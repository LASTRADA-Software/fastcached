# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of distributed compilation (Windows).
#
# The POSIX counterpart (scripts/dist-compile-e2e.sh) asserts seven properties;
# this one deliberately asserts fewer, and the reason is worth stating rather
# than leaving as an apparent omission.
#
# What is genuinely Windows-specific here is the MSVC DRIVER, and nothing else.
# The scheduler, the lease table, the slot accounting and every refusal path are
# platform-neutral code already covered by unit tests and by the POSIX fixture.
# What is NOT covered anywhere else is that an MSVC driver can be preprocessed
# for a worker and then compile that text: `DriverSpec::dispatchPreprocessFlags`
# is `/E` for MSVC, and `preprocessedInput` names the language in MSVC's own
# spelling (`/TP`, `/TC`) because the worker writes its scratch file itself and
# MSVC reads the language off that file's extension.
#
# Both of those are assertions about a driver that had never been exercised on
# this path. If either is wrong, distribution silently never works on Windows:
# every dispatched translation unit fails and is retried locally, so the build is
# correct, green, and never once helped -- which is exactly the failure the GNU
# side of this already hit, twice.
#
# So the cases here are the ones that would catch that:
#
#   1. Equivalent object -- a worker's object matches a locally compiled one.
#   2. Still a cache     -- a dispatched result is served from the cache next time.
#   3. C, not C++        -- a dispatched C translation unit comes back compiled as C.
#   4. Fingerprint       -- a worker for another toolchain is never chosen.
#
# "MATCHES" IS NOT BYTE-IDENTICAL HERE, and every part of that is measured rather
# than assumed -- the POSIX fixture asserts byte-identity and is right to, because
# an ELF object records none of this.
#
# Both MSVC drivers write the CLOCK into the COFF header: two compiles of one file
# to one path two seconds apart differ in exactly byte 4, and only `/Brepro`
# suppresses it. A fixture that passed `/Brepro` to make its own assertion true
# would be asserting something about a command line no build uses.
#
# clang-cl records one further thing, the source's BASE NAME in the COFF `.file`
# symbol -- which the worker is now told, so that difference is gone rather than
# excused. `cl` records the ABSOLUTE PATH OF THE OBJECT in `.debug$S` and hashes
# the file it opened into `.chks64`, with no debug flag asked for, and a worker
# compiles its own scratch file to its own scratch path: neither can ever match.
# Everything carrying code or data does: measured, `.text$mn`, `.rdata`, `.xdata`,
# `.pdata`, `.drectve`, `.data$r` and `.bss` are byte-identical between a reference
# compile of the original source and a worker-shaped compile of `/E` text at
# another path.
#
# So each driver asserts the strongest property it can actually carry, off a table,
# and a difference anywhere else -- a section, or a header field such as the target
# architecture -- is still a failure.
#
# Usage:
#   dist-compile-e2e.ps1 -Fastcached <path> -Node <path> -Launcher <path>
#   dist-compile-e2e.ps1 -SelfTest        # the object comparison only, no build
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime prerequisite
# was missing (skip).

param(
    [string]$Fastcached = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Node       = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcache-compile-node.exe",
    [string]$Launcher   = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcache-cc.exe",
    # Zero allocates a free block per run, which is the default; a non-zero value
    # pins one, which is what somebody reproducing a failure wants. See
    # `Get-FreePortBlock` for why the fixed default had to go.
    [int]$BasePort      = 0,
    # Exercise the object comparison against synthetic COFF input and exit.
    #
    # The comparison is the one piece of this fixture with logic of its own, and a
    # fixture's own logic is exactly what nothing else tests -- this file spent five
    # CI round trips learning that the hard way, one of them on a control that
    # compared two objects with DIFFERENT NAMES and so reported a driver as
    # non-reproducible when it is perfectly reproducible. This needs no daemon, no
    # worker and no compiler, so it runs anywhere pwsh does.
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$SKIP = 77
$exit = 0
$ranAnyCompiler = $false

# Skipped entirely under -SelfTest, which drives no process at all: requiring the
# three binaries there would make the one check that needs no build the one check
# that cannot run without one.
if (-not $SelfTest) {
    if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
    if (-not (Test-Path $Node))       { Write-Host "fastcache-compile-node not found: $Node; skipping"; exit $SKIP }
    if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }

    # Start-Process resolves a relative -FilePath against the PROCESS working
    # directory rather than PowerShell's, so a caller passing "out/build/..." would
    # get a spurious "file not found".
    $Fastcached = (Resolve-Path $Fastcached).Path
    $Node       = (Resolve-Path $Node).Path

# Every node started below turns its own cache tier OFF. `--listen-node` defaults
# to 127.0.0.1:6674 -- where `fastcache-cc` looks -- which is right for the one node
# per machine a real deployment runs and wrong here, where several share a host and
# would race for it. Said explicitly rather than left to the default's
# warn-and-continue, so a node that failed to bind for some OTHER reason still shows
# up as the fault it is.
$NoLocalCache = "--cache-memory=0"
    $Launcher   = (Resolve-Path $Launcher).Path
}

# Scratch beside the build tree, not under %TEMP%, for the reason
# run-launcher-e2e.ps1 records at length: on a GitHub runner %TEMP% is an 8.3
# short name, the two drivers disagree about it, and every root test in the
# launcher is a string prefix comparison. The reconciliation added for issue #66
# handles that now, but a fixture whose roots are ambiguous is testing the
# reconciliation as well as its own property.
$scratch = Join-Path (Split-Path (Split-Path $Launcher -Parent) -Parent) "dist-e2e"

# How many consecutive ports the run needs, counted from `$BasePort`.
#
# Nine, and the highest offset actually used is +8 (`$deadCachePort`). A block
# rather than nine independent draws because every port below is spelled as an
# offset from the base, and that arithmetic is what a reader checks against this
# number.
#
# The last one is drawn precisely so nothing ever binds it: case 5 needs a cache
# address that is refused rather than answered, and taking it from the block is
# what stops it landing on a port this run is about to bind for something else.
$PortsNeeded = 9

# Find a block of `$count` consecutive ports nothing is answering on.
#
# A connect probe, not a bind probe, for the reason the POSIX fixture's
# `free_port` gives: bind-then-close leaves the port in TIME_WAIT on some
# systems, and the caller is about to hand it to a *different* process anyway, so
# the only question this can honestly answer is "is anything answering here right
# now". Racy in principle; the test is RUN_SERIAL and the range is wide.
#
# It replaces a fixed base of 21730, which was defended on the grounds that a
# fixed port keeps the failure mode legible. It does the opposite, and what makes
# it worse here than for an ordinary fixture is WHAT holds the port: another
# `fastcached` -- leaked from an earlier run, or left by a sibling worktree. A
# daemon of the same kind does not refuse the connection; it answers. So the run
# proceeds against a cache that is not empty, the launcher's lookup returns a HIT
# for an object this run has not stored, the compile is served instead of
# dispatched, and the fixture reports "the compile was not dispatched to a
# worker" -- a message about distribution, produced by a port collision, with
# nothing anywhere naming a port. Observed exactly that way, twice, with
# `fastcache-cc: HIT` in the transcript on a run whose own daemon had just
# started.
function Get-FreePortBlock([int]$count) {
    foreach ($attempt in 1..200) {
        # Below the ephemeral range and clear of the block's own width.
        $base = Get-Random -Minimum 20000 -Maximum 39000
        $free = $true
        foreach ($offset in 0..($count - 1)) {
            $client = [System.Net.Sockets.TcpClient]::new()
            try {
                # Throws when nothing is listening, which is the answer we want.
                $client.Connect('127.0.0.1', $base + $offset)
                $free = $false
            } catch {
                # Refused: free.
            } finally {
                $client.Dispose()
            }
            if (-not $free) { break }
        }
        if ($free) { return $base }
    }
    throw "could not find $count consecutive free ports"
}

# Not under `-SelfTest`, which drives no process and opens no socket -- which is
# precisely why CMake keeps it in the DEFAULT ctest set rather than labelling it
# `smoke`. Probing here would give that case eight connect attempts it has no use
# for, and a way to fail ("could not find 8 consecutive free ports") on a machine
# whose ports are none of its business.
if (-not $SelfTest -and $BasePort -eq 0) { $BasePort = Get-FreePortBlock $PortsNeeded }

$cachePort    = $BasePort
$dispatchPort = $BasePort + 1
$workerPort   = $BasePort + 2

$procs = @()

# Quote the arguments Start-Process will not quote for you.
#
# -ArgumentList joins an array with spaces and hands the result over as ONE
# command line, so an element that CONTAINS a space arrives at the child as two
# arguments. That is not hypothetical: on a Windows runner clang-cl lives under
# `C:\Program Files\...`, so `--toolchain=C:\Program Files\LLVM\bin\clang-cl.exe`
# reached the worker as `--toolchain=C:\Program` plus a stray positional, and the
# worker refused it with "unrecognised argument" and exit 2.
#
# Applied at EVERY Start-Process here, including the ones whose arguments come
# from the build tree and therefore have no spaces on a runner today. "Safe
# because this path happens not to contain a space" is the reasoning that cost a
# CI round trip once already, and it stops being true the moment someone clones
# into `C:\Users\Someone\My Projects`.
#
# A trailing backslash before the closing quote would escape it -- the classic
# Windows quoting trap. Nothing passed here ends in a separator; a run of
# trailing backslashes would need doubling if that ever changed.
function ConvertTo-QuotedArgs([string[]]$arguments) {
    return $arguments | ForEach-Object {
        if ($_ -match '\s' -and $_ -notmatch '^"') { '"' + $_ + '"' } else { $_ }
    }
}

# Start a background process with its stderr captured.
#
# `-WindowStyle Hidden` exists to keep console windows from flashing up during a
# Windows CI run, and it is REJECTED outright by PowerShell on macOS and Linux
# ("not supported for the cmdlet 'Start-Process' on this edition"). Passing it
# unconditionally therefore made this script impossible to even structurally
# exercise anywhere but Windows -- which, for a file whose whole risk is that it
# was written without being run, is the wrong trade. Conditional here costs
# nothing on Windows and makes the orchestration runnable everywhere.
function Start-Background([string]$path, [string[]]$arguments, [string]$errorLog) {
    $common = @{
        FilePath              = $path
        ArgumentList          = (ConvertTo-QuotedArgs $arguments)
        PassThru              = $true
        RedirectStandardError = $errorLog
    }
    if ($IsWindows) { return Start-Process @common -WindowStyle Hidden }
    return Start-Process @common
}

function Stop-Spawned {
    # Every spawned process, on every exit path. One left holding a port makes the
    # NEXT run fail at startup for a reason unrelated to what actually broke.
    foreach ($p in $script:procs) {
        if ($null -eq $p) { continue }
        # Both swallow deliberately: a process that exited between the check and
        # the Kill throws, and so does WaitForExit on a handle that is already
        # gone. Cleanup runs on every exit path INCLUDING the failing ones, so
        # anything thrown here would replace the real diagnostic with a secondary
        # one about tearing down.
        try { if (-not $p.HasExited) { $p.Kill() } } catch { $null = $_ }
        try { $p.WaitForExit(5000) | Out-Null } catch { $null = $_ }
    }
    $script:procs = @()
}

function Wait-ForPort([int]$port, [System.Diagnostics.Process]$proc, [string]$what, [string]$errorLog) {
    foreach ($attempt in 1..100) {
        if ($proc.HasExited) {
            # The log, not just the code. "exited before listening (exit 2)" names
            # a whole class of startup refusals and distinguishes none of them --
            # which cost a CI round trip to learn that an argument had been split
            # in two. What the process itself said is the answer.
            if ($errorLog) { Write-Host (Read-LiveText $errorLog) }
            throw "$what exited before listening (exit $($proc.ExitCode))"
        }
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect("127.0.0.1", $port)
            $client.Close()
            return
        } catch {
            Start-Sleep -Milliseconds 200
        } finally {
            $client.Dispose()
        }
    }
    throw "$what never listened on port $port"
}

# Read a file another process is still writing to.
#
# Get-Content is not enough here. These logs belong to a worker that is STILL
# RUNNING, and on Windows opening a file another process holds can fail with a
# sharing violation depending on the FileShare mode it was opened with -- so a
# poll built on Get-Content can spin until its timeout and then report "never
# reported X" when the line was there all along. Opening with FileShare.ReadWrite
# says explicitly that a concurrent writer is expected.
#
# Returns an empty string rather than throwing when the file is missing or
# momentarily unreadable, because the caller is polling and both are ordinary.
function Read-LiveText([string]$path) {
    if (-not (Test-Path $path)) { return "" }
    try {
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
                                         [System.IO.FileAccess]::Read,
                                         [System.IO.FileShare]::ReadWrite)
        try {
            $reader = New-Object System.IO.StreamReader($stream)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } catch {
        return ""
    }
}

function Wait-ForLine([string]$path, [string]$pattern, [int]$seconds, [string]$what) {
    foreach ($attempt in 1..($seconds * 5)) {
        $text = Read-LiveText $path
        if ($text -and ($text -match $pattern)) { return $text }
        Start-Sleep -Milliseconds 200
    }
    Write-Host (Read-LiveText $path)
    throw "$what never reported /$pattern/"
}

# One translation unit whose text is unique to the caller.
#
# The tag goes in a string LITERAL rather than a comment, and that is the same
# device the POSIX fixture and check_header_move use: comments do not survive
# preprocessing, and the preprocessed text is what the key is taken over. Two
# cases with the same bytes key IDENTICALLY, so the second would open on a HIT
# against the first one's entry and pass for a reason unrelated to its property.
function New-Source([string]$root, [string]$tag) {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
    $src = Join-Path $root "u.cpp"
    @"
#include <string>
#include <vector>
namespace tagged {
struct Widget { std::string name; std::vector<int> values; };
inline int Total(Widget const& w) {
    int sum = 0;
    for (int v : w.values) sum += v;
    return sum + static_cast<int>(w.name.size());
}
}
int Entry() {
    tagged::Widget w { "$tag", { 1, 2, 3 } };
    return tagged::Total(w);
}
"@ | Set-Content -Encoding utf8 $src
    return $src
}

# Byte-identical, by hash rather than by Compare-Object.
#
# Compare-Object over two byte arrays allocates a PSObject per element, which is
# fine for a small object file and needlessly fragile as soon as one is not. The
# hash also makes the failure message useful: two digests say "these differ",
# where a Compare-Object dump says it several thousand times.
function Test-SameBytes([string]$a, [string]$b) {
    $ha = (Get-FileHash -Algorithm SHA256 -LiteralPath $a).Hash
    $hb = (Get-FileHash -Algorithm SHA256 -LiteralPath $b).Hash
    if ($ha -eq $hb) { return $true }

    # Say HOW they differ, not just that they do. This is the soundness assertion
    # of the whole feature, so its failure is the one most worth being able to act
    # on -- and equal sizes with different hashes means something quite different
    # from a size mismatch: the first says the compile embedded something
    # environment-specific, the second that it compiled something else entirely.
    $sa = (Get-Item -LiteralPath $a).Length
    $sb = (Get-Item -LiteralPath $b).Length
    Write-Host "  reference: $sa bytes, $ha"
    Write-Host "  produced:  $sb bytes, $hb"
    if ($sa -ne $sb) { return $false }

    $ba = [System.IO.File]::ReadAllBytes($a)
    $bb = [System.IO.File]::ReadAllBytes($b)
    $diffs = [System.Collections.Generic.List[int]]::new()
    for ($i = 0; $i -lt $ba.Length; $i++) {
        if ($ba[$i] -ne $bb[$i]) { [void]$diffs.Add($i) }
    }
    Write-Host ("  {0} differing byte(s); first offsets: {1}" -f $diffs.Count, (($diffs | Select-Object -First 24) -join ', '))
    return $false
}

# The sections of a COFF object, in file order, each with a digest of its bytes.
#
# Written rather than shelled out to `dumpbin` for two reasons: parsing a tool's
# prose is worse than reading the structure it describes, and this has to run
# against SYNTHETIC input in -SelfTest, where there is no object a linker would
# recognise as belonging to anything.
#
# Returns $null for an object this cannot read -- today that means the `/bigobj`
# format, whose header is a different structure entirely (Sig1 = 0x0000, Sig2 =
# 0xFFFF where an ordinary object has its Machine field). Reporting that is the
# point: silently mis-parsing it would compare two objects field by field against
# a layout neither of them has.
function Get-CoffSections([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 20) { return $null }
    if ($b[0] -eq 0 -and $b[1] -eq 0 -and $b[2] -eq 0xFF -and $b[3] -eq 0xFF) { return $null } # /bigobj

    $sectionCount = [BitConverter]::ToUInt16($b, 2)
    $symbolTable  = [BitConverter]::ToUInt32($b, 8)
    $symbolCount  = [BitConverter]::ToUInt32($b, 12)
    $optionalSize = [BitConverter]::ToUInt16($b, 16)
    $base = 20 + $optionalSize
    if ($b.Length -lt $base + 40 * $sectionCount) { return $null }

    $out = @()
    foreach ($i in 0..([int]$sectionCount - 1)) {
        $off = $base + 40 * $i
        $name = [Text.Encoding]::ASCII.GetString($b, $off, 8).TrimEnd([char]0)
        if ($name.StartsWith('/')) {
            # A name too long for the eight-byte field lives in the string table,
            # which follows the symbol table. COMDAT section names reach that length
            # routinely, so this is the ordinary case rather than an exotic one.
            $stringBase = $symbolTable + 18 * $symbolCount
            $at = $stringBase + [int]$name.Substring(1)
            if ($at -ge $b.Length) { return $null }
            $stop = $at
            while ($stop -lt $b.Length -and $b[$stop] -ne 0) { $stop++ }
            $name = [Text.Encoding]::ASCII.GetString($b, $at, $stop - $at)
        }
        $size = [BitConverter]::ToUInt32($b, $off + 16)
        $ptr  = [BitConverter]::ToUInt32($b, $off + 20)
        $hash = 'empty'
        if ($ptr -ne 0 -and $size -ne 0) {
            if ($ptr + $size -gt $b.Length) { return $null }
            $data = New-Object byte[] $size
            [Array]::Copy($b, $ptr, $data, 0, $size)
            $hash = [BitConverter]::ToString([System.Security.Cryptography.SHA256]::HashData($data)).Replace('-', '')
        }
        $out += [pscustomobject]@{ Name = $name; Size = $size; Hash = $hash }
    }
    return $out
}

# The symbol table and the string table that follows it, as one region -- and the
# check that the file is as large as its own structure says it is.
#
# That check is why this exists at all. Sections are compared by content, and a
# TRUNCATED object can lose its whole symbol table without a single section
# changing: MSVC writes the symbol table last, so nine-tenths of a file still
# parses, still has every section intact, and still compares equal. A truncated
# transfer is one of the few faults distribution can actually introduce, so a
# comparison that accepts one is worth very little.
#
# COFF states its own end: the string table opens with a four-byte size that
# INCLUDES those four bytes, and it is the last thing in the file. If that number
# and the bytes actually present disagree, the object is damaged, whatever its
# sections say.
#
# Returns $null for a damaged or unreadable object.
function Get-CoffTail([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 20) { return $null }
    $ptr   = [BitConverter]::ToUInt32($b, 8)
    $count = [BitConverter]::ToUInt32($b, 12)
    if ($ptr -eq 0) { return [pscustomobject]@{ Present = $false; Length = 0; Hash = 'none' } }

    $stringsAt = $ptr + 18 * $count
    if ($stringsAt + 4 -gt $b.Length) { return $null }
    $declared = [BitConverter]::ToUInt32($b, $stringsAt)
    if ($stringsAt + $declared -ne $b.Length) { return $null }

    $length = $b.Length - $ptr
    $data = New-Object byte[] $length
    [Array]::Copy($b, $ptr, $data, 0, $length)
    return [pscustomobject]@{
        Present = $true
        Length  = $length
        Hash    = [BitConverter]::ToString([System.Security.Cryptography.SHA256]::HashData($data)).Replace('-', '')
    }
}

# The COFF header, field by field, and which fields may differ between two
# compiles of the same code.
#
# TimeDateStamp is the clock, and both MSVC-family drivers write it: measured, two
# compiles of one file to one path two seconds apart differ in exactly byte 4, and
# `/Brepro` is what suppresses it. So no MSVC object is ever byte-identical to one
# compiled in a different second, by anybody, distribution or not -- and the
# fixture must not pass `/Brepro` to make its own assertion true, because then it
# would be asserting something about a command line no build uses.
#
# PointerToSymbolTable is a FILE OFFSET, so it moves whenever an excused section
# changes size, which on `cl` it always does. Excusing the record but not the
# offset it shifts would fail every comparison for the reason it just excused.
#
# Everything else is compared, and Machine is why this is a table rather than a
# skip: an object built for another architecture differs there and NOWHERE else
# that a section walk would notice, which is exactly the class of wrongness this
# whole fixture exists to catch.
$CoffHeaderFields = @(
    @{ Name = "Machine";              Offset = 0;  Size = 2; MayDiffer = $false }
    @{ Name = "NumberOfSections";     Offset = 2;  Size = 2; MayDiffer = $false }
    @{ Name = "TimeDateStamp";        Offset = 4;  Size = 4; MayDiffer = $true  }
    @{ Name = "PointerToSymbolTable"; Offset = 8;  Size = 4; MayDiffer = $true  }
    @{ Name = "NumberOfSymbols";      Offset = 12; Size = 4; MayDiffer = $false }
    @{ Name = "SizeOfOptionalHeader"; Offset = 16; Size = 2; MayDiffer = $false }
    @{ Name = "Characteristics";      Offset = 18; Size = 2; MayDiffer = $false }
)

# Does the produced object match the reference, by this driver's own standard?
#
# `$mayDiffer` names the SECTIONS whose content is the compiler's record of WHERE
# it compiled rather than WHAT it compiled, and it is empty for a driver that
# records no such thing. Everything outside it must match byte for byte, in the
# same order and under the same names, so the exclusion cannot widen quietly: a
# section that appears, disappears, or moves is a failure however it is spelled.
#
# What the exclusions cannot hide is covered elsewhere. A worker running a
# DIFFERENT COMPILER is caught by the fingerprint the fixture already asserts
# agreement on; a different ARCHITECTURE by the header table above; and different
# CODE by `.text$mn` -- the three things `.debug$S` could otherwise be imagined to
# be concealing.
function Test-EquivalentObject([string]$reference, [string]$produced, [hashtable]$rules) {
    if (Test-SameBytes $reference $produced) { return $true }
    $mayDiffer = [string[]]$rules.Sections

    $ba = [System.IO.File]::ReadAllBytes($reference)
    $bb = [System.IO.File]::ReadAllBytes($produced)
    if ($ba.Length -lt 20 -or $bb.Length -lt 20) {
        Write-Host "  one of these is too short to be a COFF object"
        return $false
    }
    foreach ($field in $CoffHeaderFields) {
        if ($field.MayDiffer) { continue }
        $x = if ($field.Size -eq 2) { [BitConverter]::ToUInt16($ba, $field.Offset) } else { [BitConverter]::ToUInt32($ba, $field.Offset) }
        $y = if ($field.Size -eq 2) { [BitConverter]::ToUInt16($bb, $field.Offset) } else { [BitConverter]::ToUInt32($bb, $field.Offset) }
        if ($x -ne $y) {
            Write-Host ("  COFF header field {0} differs: {1} vs {2}" -f $field.Name, $x, $y)
            return $false
        }
    }

    $sa = Get-CoffSections $reference
    $sb = Get-CoffSections $produced
    if ($null -eq $sa -or $null -eq $sb) {
        Write-Host "  one of these is not an ordinary COFF object (/bigobj?); cannot compare section by section"
        return $false
    }

    $excused = @()
    foreach ($i in 0..($sa.Count - 1)) {
        if ($sa[$i].Name -ne $sb[$i].Name) {
            Write-Host ("  section {0} is '{1}' in one and '{2}' in the other" -f $i, $sa[$i].Name, $sb[$i].Name)
            return $false
        }
        if ($sa[$i].Hash -eq $sb[$i].Hash) { continue }
        if ($mayDiffer -contains $sa[$i].Name) {
            $excused += $sa[$i].Name
            continue
        }
        Write-Host ("  section '{0}' differs ({1} vs {2} bytes) -- that is code or data, not a path record" `
                    -f $sa[$i].Name, $sa[$i].Size, $sb[$i].Size)
        return $false
    }
    # The symbol table and string table, which no section walk covers -- and which a
    # truncated object loses entirely while every section still compares equal.
    #
    # Both objects are checked for structural self-consistency whatever the rules
    # say, because "the file is as large as it claims" is not a property any driver
    # gets to opt out of. Whether the CONTENT may differ is a per-driver row:
    # measured, clang-cl's tail is byte-identical between a local compile and a
    # worker-shaped one, and `cl`'s is not -- same length, different bytes -- so
    # the stronger claim is made exactly where it holds.
    $ta = Get-CoffTail $reference
    $tb = Get-CoffTail $produced
    if ($null -eq $ta -or $null -eq $tb) {
        Write-Host "  one of these is damaged: its string table does not end where the file does (a truncated transfer?)"
        return $false
    }
    if ($ta.Length -ne $tb.Length) {
        Write-Host ("  the symbol and string tables differ in SIZE: {0} vs {1} bytes" -f $ta.Length, $tb.Length)
        return $false
    }
    if (-not $rules.TailMayDiffer -and $ta.Hash -ne $tb.Hash) {
        Write-Host "  the symbol and string tables differ, and this driver records nothing there that may"
        return $false
    }

    $what = if ($excused.Count -eq 0) { "the clock" }
            else { "the clock and " + (($excused | Select-Object -Unique) -join ', ') }
    Write-Host ("  identical apart from {0}, which record when and where the compile happened" -f $what)
    return $true
}

# The cache port is a PARAMETER, not read from the enclosing scope.
#
# It was the latter, and the isolation case below tried to override
# $env:FASTCACHE_ADDR around the call -- which this function then clobbered on its
# own first line, so that case silently used the MAIN cache while talking to the
# isolation scheduler. It would still have passed, for the wrong reason, which is
# the failure mode every fixture here is written to avoid.
# Can this driver actually compile, or is it merely on PATH?
#
# `Get-Command` answers presence, which is not the question. A clang-cl that
# cannot find an MSVC SDK is on PATH and cannot build anything, and so is one on
# a machine where the Visual Studio environment was never sourced -- both would
# turn this fixture into a red build reporting "the reference compile failed",
# which describes the runner rather than the code under test.
#
# A missing runtime prerequisite is a SKIP, and this is what makes the two
# distinguishable. It also happens to make the script runnable to a clean
# conclusion on a developer machine, where clang-cl exists, targets MSVC, and has
# no SDK to target it with.
function Test-CompilerWorks([string]$compiler, [string]$where) {
    $probeDir = Join-Path $where "probe"
    New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
    $probeSrc = Join-Path $probeDir "probe.cpp"
    $probeObj = Join-Path $probeDir "probe.obj"
    @'
#include <string>
int Probe() { return static_cast<int>(std::string("x").size()); }
'@ | Set-Content -Encoding utf8 $probeSrc
    & $compiler /nologo /c "/Fo$probeObj" $probeSrc 2>&1 | Out-Null
    return (Test-Path $probeObj)
}

function Invoke-Dispatching([string]$compiler, [string]$root, [string]$obj,
                            [string]$scheduler, [int]$cache, [string]$sourceName = "u.cpp") {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$cache"
    $env:FASTCACHE_SOURCE_DIR = $root
    $env:FASTCACHE_BINARY_DIR = (Join-Path $root "build")
    $env:FASTCACHE_VERBOSE    = "1"
    if ($scheduler) { $env:FASTCACHE_SCHEDULER = $scheduler }
    else            { Remove-Item -Path "env:FASTCACHE_SCHEDULER" -ErrorAction SilentlyContinue }

    $source  = Join-Path $root $sourceName
    $errFile = New-TemporaryFile
    $p = Start-Process -FilePath $Launcher `
        -ArgumentList (ConvertTo-QuotedArgs @($compiler, "/nologo", "/c", "/Fo$obj", $source)) `
        -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stderr = $err }
}
# --- the object comparison, tested against input it can be given on purpose ----
#
# A COFF object built by hand, so the cases below can differ in exactly one thing.
# Real objects cannot: two `cl` runs differ in their object PATH and their source
# CHECKSUM together, which is precisely the entanglement that made a hand-run
# control report a reproducible driver as non-reproducible.
function New-SyntheticCoff([string]$path, [object[]]$sections, [uint32]$stamp = 0, [uint16]$machine = 0x8664, [string]$strings = "") {
    $rows = @($sections)
    $header = New-Object byte[] 20
    [Array]::Copy([BitConverter]::GetBytes($machine), 0, $header, 0, 2)
    [Array]::Copy([BitConverter]::GetBytes([uint16]$rows.Count), 0, $header, 2, 2)
    [Array]::Copy([BitConverter]::GetBytes($stamp), 0, $header, 4, 4)
    # The symbol table pointer, the symbol count, the optional header size and the
    # characteristics stay zero: nothing here reads them except the long-name path,
    # which these names deliberately do not take.

    $tableBytes = 40 * $rows.Count
    $headers = New-Object byte[] $tableBytes
    $blob = [System.Collections.Generic.List[byte]]::new()
    $cursor = 20 + $tableBytes
    foreach ($i in 0..($rows.Count - 1)) {
        $name = [Text.Encoding]::ASCII.GetBytes($rows[$i].Name)
        [Array]::Copy($name, 0, $headers, 40 * $i, [Math]::Min(8, $name.Length))
        $bytes = [byte[]]$rows[$i].Bytes
        [Array]::Copy([BitConverter]::GetBytes([uint32]$bytes.Length), 0, $headers, 40 * $i + 16, 4)
        [Array]::Copy([BitConverter]::GetBytes([uint32]$cursor), 0, $headers, 40 * $i + 20, 4)
        $blob.AddRange($bytes)
        $cursor += $bytes.Length
    }
    # A string table, when asked for: NumberOfSymbols stays zero and the table is
    # just its own four-byte size followed by the text, which is the shape
    # Get-CoffTail validates a real object against.
    $tail = [System.Collections.Generic.List[byte]]::new()
    if ($strings -ne "") {
        $text = [Text.Encoding]::ASCII.GetBytes($strings)
        $tail.AddRange([BitConverter]::GetBytes([uint32]($text.Length + 4)))
        $tail.AddRange($text)
        [Array]::Copy([BitConverter]::GetBytes([uint32]$cursor), 0, $header, 8, 4)   # PointerToSymbolTable
    }

    $all = [System.Collections.Generic.List[byte]]::new()
    $all.AddRange($header); $all.AddRange($headers); $all.AddRange($blob); $all.AddRange($tail)
    [System.IO.File]::WriteAllBytes($path, $all.ToArray())
}

function Invoke-SelfTest {
    $failures = 0
    function Assert-That([bool]$condition, [string]$what) {
        if ($condition) { Write-Host "   ok   $what" }
        else { Write-Host "   FAIL $what"; $script:selfTestFailures++ }
    }
    $script:selfTestFailures = 0

    $dir = Join-Path ([System.IO.Path]::GetTempPath()) ("dist-e2e-selftest-" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    try {
        $code    = [byte[]](1..64)
        $other   = [byte[]](65..128)
        # Stand-ins for what `cl` writes into `.debug`$S`: the object's own path,
        # equal in length so the case turns on content rather than on size.
        $record  = [Text.Encoding]::ASCII.GetBytes("C-build-one-tu.o")
        $record2 = [Text.Encoding]::ASCII.GetBytes("C-build-two-tu.o")

        $a = Join-Path $dir "a.obj"; $b = Join-Path $dir "b.obj"

        # The two standards this fixture applies, as the driver table spells them.
        $loose  = @{ Sections = @(".debug`$S", ".chks64"); TailMayDiffer = $true }
        $strict = @{ Sections = @();                      TailMayDiffer = $false }

        # Identical objects match under every standard, and the byte comparison is
        # what answers -- the section walk is never reached.
        New-SyntheticCoff $a @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".debug`$S"; Bytes = $record })
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".debug`$S"; Bytes = $record })
        Assert-That (Test-EquivalentObject $a $b $strict) "identical objects match under the strict standard"
        Assert-That (Test-EquivalentObject $a $b $loose) "identical objects match with sections excused"

        # The real MSVC case: same code, a different record of where it was written.
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".debug`$S"; Bytes = $record2 })
        Assert-That (Test-EquivalentObject $a $b $loose) "a differing path record is excused when the driver embeds one"
        Assert-That (-not (Test-EquivalentObject $a $b $strict)) "and is NOT excused for a driver that embeds none"

        # The case the whole assertion exists for: different code.
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $other }, @{ Name = ".debug`$S"; Bytes = $record })
        Assert-That (-not (Test-EquivalentObject $a $b $loose)) "differing code fails even with sections excused"

        # A section that appears, disappears or is renamed is a failure however the
        # rest compares: the excuse names sections, so it must not widen to a shape.
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code })
        Assert-That (-not (Test-EquivalentObject $a $b $loose)) "a missing section fails"
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".chks64"; Bytes = $record })
        Assert-That (-not (Test-EquivalentObject $a $b $loose)) "a renamed section fails"

        # The clock, which both MSVC drivers write and neither can be asked not to
        # without a flag no build passes: excused for every driver, so an object
        # differing ONLY there matches even under the strict standard.
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".debug`$S"; Bytes = $record }) 0x67890123
        Assert-That (Test-EquivalentObject $a $b $strict) "a differing timestamp is excused for every driver"

        # The architecture, which differs THERE and nowhere a section walk would
        # see -- the reason the header is a table rather than a skip.
        New-SyntheticCoff $b @(@{ Name = ".text`$mn"; Bytes = $code }, @{ Name = ".debug`$S"; Bytes = $record }) 0 0x014C
        Assert-That (-not (Test-EquivalentObject $a $b $loose)) "a different target architecture fails"

        # The symbol and string tables, which no section walk covers. Measured:
        # clang-cl's are byte-identical between a local compile and a worker-shaped
        # one, `cl`'s are not -- so the strict standard compares them and the loose
        # one does not.
        $withTail  = Join-Path $dir "tail-a.obj"
        $withTail2 = Join-Path $dir "tail-b.obj"
        New-SyntheticCoff $withTail  @(@{ Name = ".text`$mn"; Bytes = $code }) 0 0x8664 "symbols-one"
        New-SyntheticCoff $withTail2 @(@{ Name = ".text`$mn"; Bytes = $code }) 0 0x8664 "symbols-two"
        Assert-That (-not (Test-EquivalentObject $withTail $withTail2 $strict)) "a differing symbol table fails the strict standard"
        Assert-That (Test-EquivalentObject $withTail $withTail2 $loose) "and is excused where the driver records paths there"

        # TRUNCATION, which is what all of the above misses: MSVC writes the symbol
        # table last, so a cut-off object keeps every section intact and compares
        # equal section by section. COFF states its own end, and that is what says
        # otherwise.
        $cut = Join-Path $dir "cut.obj"
        $bytes = [System.IO.File]::ReadAllBytes($withTail)
        [System.IO.File]::WriteAllBytes($cut, $bytes[0..($bytes.Length - 4)])
        Assert-That ($null -eq (Get-CoffTail $cut)) "a truncated object is recognised as damaged"
        Assert-That (-not (Test-EquivalentObject $withTail $cut $loose)) "and fails even under the loosest standard"

        # And the format this cannot read is reported rather than mis-parsed.
        $big = Join-Path $dir "big.obj"
        [System.IO.File]::WriteAllBytes($big, [byte[]](0x00, 0x00, 0xFF, 0xFF) + (New-Object byte[] 64))
        Assert-That ($null -eq (Get-CoffSections $big)) "a /bigobj header is refused rather than mis-parsed"

        # The parse itself, since everything above rests on it.
        $sections = Get-CoffSections $a
        Assert-That ($sections.Count -eq 2) "both sections are found"
        Assert-That ($sections[0].Name -eq ".text`$mn" -and $sections[1].Name -eq ".debug`$S") "in file order, by name"
        Assert-That ($sections[0].Size -eq $code.Length) "with their sizes"
    } finally {
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    }

    if ($script:selfTestFailures -ne 0) {
        Write-Host "object-comparison self-test FAILED ($script:selfTestFailures)"
        return 1
    }
    Write-Host "object-comparison self-test PASSED"
    return 0
}

if ($SelfTest) { exit (Invoke-SelfTest) }


# What "the same object" means, per driver, and why it is not one answer.
#
# Measured on MSVC 14.51 and clang-cl, by compiling the same text at different
# paths and comparing section by section:
#
#   clang-cl records the source's BASE NAME (the COFF `.file` symbol) and nothing
#   else about where it ran, and the worker is told that name -- so nothing but the
#   clock may differ, and an empty row is what says so. That strictness is load-
#   bearing rather than tidy: it is what fails, end to end, if the client ever
#   stops telling the worker what to call its scratch file. Verified by making it
#   stop.
#
#   cl additionally writes the ABSOLUTE PATH OF THE OBJECT into `.debug$S`, with no
#   debug flag asked for, and `.chks64` hashes the file it actually opened. A worker
#   compiles its own scratch file to its own scratch path, so neither can ever
#   match; every other section does, and those are the ones carrying code.
#
# A driver that recorded something new would fail here rather than being excused by
# a rule written wide enough to cover it in advance.
$Drivers = @(
    @{ Name = "cl";       Rules = @{ Sections = @(".debug`$S", ".chks64"); TailMayDiffer = $true } }
    @{ Name = "clang-cl"; Rules = @{ Sections = @();                       TailMayDiffer = $false } }
)

try {
    foreach ($driver in $Drivers) {
        $cc = $driver.Name
        $rules = $driver.Rules
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
            Write-Host "skip $cc (not on PATH)"
            continue
        }

        if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
        New-Item -ItemType Directory -Force -Path $scratch | Out-Null

        # The cluster key every node here shares, and what makes this fixture's
        # dispatch path the SIGNED one: without it the scheduler hands out bare
        # serials and every worker runs the unchecked validator, so a run that
        # dispatches this many compiles would exercise none of the lease check
        # (#282). The shell twin does the same, with the same fixed text -- nothing
        # here turns on the value, and a per-run secret would make a failure look
        # like a flake.
        $clusterKey = Join-Path $scratch "cluster.key"
        Set-Content -Path $clusterKey -Value "e2e-fixture-cluster-key-not-a-secret" -NoNewline

        if (-not (Test-CompilerWorks $cc $scratch)) {
            Write-Host "skip $cc (on PATH but cannot compile here)"
            continue
        }
        $ranAnyCompiler = $true
        Write-Host "== driver: $cc"

        # Statistics are per-user state; keep this run out of the developer's log.
        $env:LOCALAPPDATA = Join-Path $scratch "state"
        New-Item -ItemType Directory -Force -Path $env:LOCALAPPDATA | Out-Null

        # One listener now, and only the cache. `fastcached` used to carry the
        # scheduler too, on a second `--listen-dispatch` endpoint; that flag is gone.
        # Handing out capacity is a decision only ONE node may make at a time, and
        # nothing in the cache daemon can establish which node that is -- so the
        # scheduler moved to where cluster leadership lives, which is the node.
        $daemonLog = Join-Path $scratch "daemon.log"
        $daemon = Start-Background $Fastcached @(
            "--listen=127.0.0.1:$cachePort",
            "--storage-max-value=64M", "--log-level=info") $daemonLog
        $procs += $daemon
        Wait-ForPort $cachePort $daemon "daemon" $daemonLog

        # A compile node running the fleet's scheduler. --fleet-open because every
        # peer here is loopback -- and because the policy has to be STATED: a node
        # with no member list refuses everybody, which is the right default and not a
        # working configuration, so it is refused at startup.
        #
        # It names a toolchain nothing here compiles with, deliberately. Every node is
        # both a peer and a possible scheduler, so it always registers as a worker too
        # -- and a second MATCHING worker would make "which worker ran this job" a race
        # that the cases below assert against by reading one worker's counters.
        # One port: since #290 stage 3 the compile verbs arrive on --listen-node
        # beside the cache and scheduler verbs, so this node's worker half answers on
        # $dispatchPort too and --advertise names that. The dedicated compile port it
        # used to open, and the $BasePort + 6 it used to take, are both gone.
        $schedLog = Join-Path $scratch "scheduler.log"
        $scheduler = Start-Background $Node @(
            $NoLocalCache, "--cluster-key-file=$clusterKey",
            "--serve-scheduler", "--listen-node=127.0.0.1:$dispatchPort", "--fleet-open",
            "--scheduler=127.0.0.1:$dispatchPort",
            "--advertise=127.0.0.1:$dispatchPort",
            "--toolchain=scheduler-only=$((Get-Command $cc).Source)", "--slots=1",
            "--log-level=debug") $schedLog
        $procs += $scheduler
        Wait-ForPort $dispatchPort $scheduler "scheduler" $schedLog

        # Asked of the launcher rather than derived here. The fingerprint is a
        # digest over the compiler's whole include tree; a fixture that recomputed
        # it would assert its own reimplementation, and if the two disagreed every
        # case would degrade to a local compile and still exit 0 -- passing while
        # testing nothing.
        $ccPath = (Get-Command $cc).Source
        $fingerprint = (& $Launcher --print-toolchain-fingerprint $ccPath) | Select-Object -First 1
        if (-not $fingerprint) { throw "the launcher reported no toolchain fingerprint for $cc" }

        # Slots enough that background CPU cannot withdraw all of them.
        #
        # `AvailableSlots` reduces a worker's ceiling by the cores its machine is
        # busy with OUTSIDE this fleet -- `cpuBusyPermille * logicalCores / 1000`,
        # less this fleet's own in-flight jobs -- so a worker offering two slots on
        # a many-core machine withdraws both as soon as a few percent of that
        # machine is doing something else. This fixture IS that something else: it
        # runs local reference compiles on the same box, and on CI the rest of the
        # suite runs beside it. The dispatch then comes back `rejected (withdrawn)`
        # and the case fails as "the compile was not dispatched to a worker", which
        # reads as a fault in dispatch and is a fault in the fixture's sizing.
        #
        # Offering the whole machine puts the ceiling at cores-minus-external,
        # which reaches zero only when the host really is saturated -- and is what
        # a node dedicating this machine to the fleet would advertise anyway. The
        # `--slots=1` workers elsewhere in this file are deliberate and stay: their
        # cases are ABOUT a worker having exactly one.
        $workerSlots = [Environment]::ProcessorCount

        $workerLog = Join-Path $scratch "worker.log"
        $worker = Start-Background $Node @(
            $NoLocalCache, "--cluster-key-file=$clusterKey",
            "--scheduler=127.0.0.1:$dispatchPort", "--listen-node=127.0.0.1:$workerPort",
            "--advertise=127.0.0.1:$workerPort", "--toolchain=$ccPath", "--slots=$workerSlots",
            "--log-level=debug") $workerLog
        $procs += $worker
        Wait-ForPort $workerPort $worker "worker" $workerLog
        $workerText = Wait-ForLine $workerLog "toolchain\(s\) registered" 120 "worker"

        # The worker computed its own fingerprint from a bare --toolchain. If it
        # derived a different digest from the launcher's, everything below still
        # "works" -- it registers, it heartbeats, the scheduler never matches it,
        # and every case falls back to a local compile and exits 0.
        if ($workerText -notmatch [regex]::Escape($fingerprint)) {
            throw "worker and launcher disagree on the toolchain fingerprint (launcher: $fingerprint)"
        }
        Write-Host "   fingerprint agreed by launcher and worker"

        # --- 1 + 2: an equivalent object, then served from the cache ---------
        $root = Join-Path $scratch "proj"
        $src  = New-Source $root "$cc-dist-case-one"
        $refObj = Join-Path $root "build\reference.obj"
        $obj    = Join-Path $root "build\u.obj"

        # Compiled to the object path the LAUNCHER will write, then moved aside.
        #
        # Not tidiness: `cl` records the absolute path of the object inside the
        # object, so a reference built as `reference.obj` differs from one built as
        # `u.obj` in that record alone -- which would put a difference into every
        # comparison below that has nothing to do with distribution, and did.
        & $cc /nologo /c "/Fo$obj" $src | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the reference compile failed" }
        Move-Item -LiteralPath $obj -Destination $refObj -Force

        $r = Invoke-Dispatching $cc $root $obj "127.0.0.1:$dispatchPort" $cachePort
        if ($r.code -ne 0) { Write-Host $r.stderr; throw "the dispatched compile failed" }
        if ($r.stderr -notmatch "DISPATCHED to ") {
            Write-Host $r.stderr
            # The WORKER's log too, not just the client's. A refusal reaches the
            # client as one line naming a wire error code, and the reason it
            # happened -- an unwritable scratch directory, a compiler that will not
            # start -- is only ever visible on the worker. Printing one without the
            # other is how a dispatch failure turns into a round trip.
            Write-Host "--- worker log ---"
            Write-Host (Read-LiveText $workerLog)
            throw "the compile was not dispatched to a worker"
        }
        if (-not (Test-Path $obj)) { throw "no object was written by the dispatched compile" }

        # The whole soundness claim: an object built on the worker from
        # `/E`-preprocessed text must match one this machine compiled directly, by
        # this driver's own standard of matching.
        if (-not (Test-EquivalentObject $refObj $obj $rules)) {
            # THE CONTROL, and it answers the question the failure raises rather
            # than the one it looks like. The reference compiles the ORIGINAL
            # source; the worker compiles `/E`-preprocessed text. Those are
            # different inputs, so a difference between them does not yet say
            # whether distribution is at fault -- it might be inherent to compiling
            # preprocessed text on this driver.
            Write-Host "--- control: preprocess and compile locally, as the worker does ---"
            $ctlDir = Join-Path $scratch "control"
            New-Item -ItemType Directory -Force -Path $ctlDir | Out-Null
            $ctlSrc = Join-Path $ctlDir (Split-Path $src -Leaf)
            $ctlObj = Join-Path $ctlDir "control.obj"
            & $cc /nologo /E $src 2>$null | Set-Content -Encoding utf8 $ctlSrc
            if ($LASTEXITCODE -ne 0) {
                Write-Host "  control preprocess failed; inconclusive"
            } else {
                & $cc /nologo -c $ctlSrc "/Fo$ctlObj" 2>&1 | Out-Null
                if (-not (Test-Path $ctlObj)) {
                    Write-Host "  control compile produced no object; inconclusive"
                } elseif (Test-EquivalentObject $ctlObj $obj $rules) {
                    Write-Host "  control MATCHES the worker: the difference is preprocessed-vs-original input,"
                    Write-Host "  not the worker's environment."
                } else {
                    Write-Host "  control DIFFERS from the worker too."

                    # Is this driver reproducible at all? Compile the identical
                    # input a second time TO THE SAME PATH, keeping the first
                    # result aside.
                    #
                    # The same path is the whole point, and the previous version of
                    # this control got it wrong: it compiled to `tu2.o` and compared
                    # against `tu.o`, so the object NAME differed -- which `cl`
                    # records inside the object -- and it reported a perfectly
                    # reproducible driver as non-reproducible, in a CI log, as the
                    # answer to the question this fixture had been asking for three
                    # commits.
                    Write-Host "--- control 2: is this driver even reproducible? ---"
                    $ctlKeep = Join-Path $ctlDir "control-first.obj"
                    Move-Item -LiteralPath $ctlObj -Destination $ctlKeep -Force
                    & $cc /nologo -c $ctlSrc "/Fo$ctlObj" 2>&1 | Out-Null
                    if (-not (Test-Path $ctlObj)) {
                        Write-Host "  second control compile produced no object; inconclusive"
                    } elseif (Test-SameBytes $ctlKeep $ctlObj) {
                        Write-Host "  two identical local compiles MATCH: the driver is reproducible,"
                        Write-Host "  so the worker really is leaking its environment into the object."
                    } else {
                        Write-Host "  two identical local compiles DIFFER at the same path: this driver does"
                        Write-Host "  not produce reproducible objects at all, and the assertion -- not the"
                        Write-Host "  product -- is what needs to change."
                    }
                }
            }
            throw "the worker's object differs from the locally compiled one"
        }
        Write-Host "   the worker's object matches the local one"

        Remove-Item $obj -Force
        $r = Invoke-Dispatching $cc $root $obj "127.0.0.1:$dispatchPort" $cachePort
        if ($r.code -ne 0) { Write-Host $r.stderr; throw "the second compile failed" }
        if ($r.stderr -notmatch "fastcache-cc: HIT") {
            Write-Host $r.stderr
            throw "a dispatched result was not served from the cache afterwards"
        }
        if ($r.stderr -match "DISPATCHED to ") {
            Write-Host $r.stderr
            throw "a cached compile was dispatched again"
        }
        Write-Host "   served from the cache on the second compile"

        # --- 3: a C translation unit comes back compiled as C --------------
        #
        # The worker names its own scratch file, and an MSVC driver reads the
        # language off that name -- so while nothing stated the language, a
        # dispatched `.c` was compiled as C++: a failed remote compile where C is
        # not valid C++ (distribution silently never helping, with a green build),
        # and an object with C++ mangling stored under the C key where it is.
        #
        # WHAT THIS CASE GUARDS, established by reintroducing each defect and
        # watching which leg fails, because two independent things now prevent it
        # and a case that cannot tell them apart is worth stating precisely:
        #
        #   - with `/TC`//`/TP` removed AND the client's source name no longer sent,
        #     this case fails (the remote compile of C-as-C++ fails outright, so
        #     nothing is dispatched);
        #   - with only the source name unsent, `/TC` carries the language and this
        #     case still passes -- which is the point of stating it explicitly
        #     rather than letting it ride on a file name;
        #   - with only `/TC` removed, the name carries it instead, and the CASE 1
        #     leg on clang-cl is what fails, because its symbol table then records
        #     a name this machine never compiled.
        #
        # So the two mechanisms are guarded by two different legs, and neither is
        # merely redundant. Which of them a given driver relies on is exactly what
        # must not matter, and that is why both exist.
        $croot = Join-Path $scratch "cproj"
        New-Item -ItemType Directory -Force -Path (Join-Path $croot "build") | Out-Null
        $csrc = Join-Path $croot "u.c"
        # The tag is a string LITERAL, as New-Source explains at length: a comment
        # does not survive preprocessing, and two cases whose preprocessed text is
        # identical key identically -- so the second would open on the first one's
        # entry and pass for a reason unrelated to its property.
        @"
#include <stddef.h>
static const char Tag[] = "$cc-dist-case-c";
static int Helper(int v) { return v + (int) sizeof(Tag); }
int Entry(void) { return Helper((int) sizeof(size_t)); }
"@ | Set-Content -Encoding utf8 $csrc

        $cref = Join-Path $croot "build\reference.obj"
        $cobj = Join-Path $croot "build\u.obj"
        & $cc /nologo /c "/Fo$cobj" $csrc | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the C reference compile failed" }
        Move-Item -LiteralPath $cobj -Destination $cref -Force

        $r = Invoke-Dispatching $cc $croot $cobj "127.0.0.1:$dispatchPort" $cachePort "u.c"
        if ($r.code -ne 0) { Write-Host $r.stderr; throw "the dispatched C compile failed" }
        if ($r.stderr -notmatch "DISPATCHED to ") {
            Write-Host $r.stderr
            Write-Host "--- worker log ---"
            Write-Host (Read-LiveText $workerLog)
            throw "the C compile was not dispatched to a worker"
        }
        if (-not (Test-Path $cobj)) { throw "no object was written by the dispatched C compile" }
        # C compiled as C++ differs in far more than a path record: the symbols are
        # mangled, so `.text$mn` and the symbol table both move.
        if (-not (Test-EquivalentObject $cref $cobj $rules)) {
            throw "a dispatched C translation unit did not come back compiled as C"
        }
        Write-Host "   a C translation unit was compiled as C on the worker"

        # --- 4: a worker for another toolchain is never chosen ---------------
        # Its own daemon and its own worker, so the mismatched worker is the ONLY
        # one registered. Reusing the fleet above would leave a matching worker
        # available and the case would pass without testing anything.
        $isoCache    = $BasePort + 3
        $isoDispatch = $BasePort + 4
        $isoWorker   = $BasePort + 5

        $isoDaemonLog = Join-Path $scratch "iso-daemon.log"
        $isoDaemon = Start-Background $Fastcached @(
            "--listen=127.0.0.1:$isoCache",
            "--log-level=info") $isoDaemonLog
        $procs += $isoDaemon
        Wait-ForPort $isoCache $isoDaemon "isolation daemon" $isoDaemonLog

        # A second SCHEDULER as well, so the mismatched worker is the only one
        # registered with it -- and it too serves a toolchain nothing here uses, or it
        # would BE a matching worker and the case would pass without testing anything.
        $isoSchedLog = Join-Path $scratch "iso-scheduler.log"
        $isoScheduler = Start-Background $Node @(
            $NoLocalCache, "--cluster-key-file=$clusterKey",
            "--serve-scheduler", "--listen-node=127.0.0.1:$isoDispatch", "--fleet-open",
            "--scheduler=127.0.0.1:$isoDispatch",
            "--advertise=127.0.0.1:$isoDispatch",
            "--toolchain=also-not-the-compiler-this-client-uses=$((Get-Command $cc).Source)",
            "--slots=1", "--log-level=debug") $isoSchedLog
        $procs += $isoScheduler
        Wait-ForPort $isoDispatch $isoScheduler "isolation scheduler" $isoSchedLog

        $isoWorkerLog = Join-Path $scratch "iso-worker.log"
        $isoNode = Start-Background $Node @(
            $NoLocalCache, "--cluster-key-file=$clusterKey",
            "--scheduler=127.0.0.1:$isoDispatch", "--listen-node=127.0.0.1:$isoWorker",
            "--advertise=127.0.0.1:$isoWorker",
            "--toolchain=not-the-compiler-this-client-uses=$ccPath", "--slots=2",
            "--log-level=debug") $isoWorkerLog
        $procs += $isoNode
        Wait-ForPort $isoWorker $isoNode "isolation worker" $isoWorkerLog
        Wait-ForLine $isoWorkerLog "toolchain\(s\) registered" 120 "isolation worker" | Out-Null

        $isoRoot = Join-Path $scratch "iso-proj"
        $isoSrc  = New-Source $isoRoot "$cc-dist-case-three"
        $isoRef  = Join-Path $isoRoot "build\reference.obj"
        $isoObj  = Join-Path $isoRoot "build\u.obj"
        # Compiled to the launcher's own output path and moved aside, as case 1
        # does and for the same reason: `cl` records that path inside the object,
        # so a reference built under another name differs from the fallback in the
        # record alone. Both objects here are local compiles of one source by one
        # driver to one path, so the ONLY thing left that may differ is the clock.
        & $cc /nologo /c "/Fo$isoObj" $isoSrc | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the case 4 reference compile failed" }
        Move-Item -LiteralPath $isoObj -Destination $isoRef -Force

        $r = Invoke-Dispatching $cc $isoRoot $isoObj "127.0.0.1:$isoDispatch" $isoCache
        if ($r.code -ne 0) { Write-Host $r.stderr; throw "the compile failed with only a mismatched worker" }
        if ($r.stderr -match "DISPATCHED to ") {
            Write-Host $r.stderr
            throw "a job was dispatched to a worker with a different toolchain"
        }
        if ($r.stderr -notmatch "not dispatched \(rejected \(no-worker\)") {
            Write-Host $r.stderr
            throw "expected a no-worker refusal naming the missing toolchain"
        }
        if (-not (Test-EquivalentObject $isoRef $isoObj $rules)) {
            throw "the locally compiled fallback object does not match the reference"
        }
        Write-Host "   a mismatched worker was refused, and the build compiled locally"

        # --- 5: an unreachable cache does not take the fleet with it ---------
        #
        # THE regression case for issue #236 -- `RunCached` returned on a fetch
        # that failed at the transport, above the call site that would dispatch,
        # so a cache the launcher could not reach turned off distribution too. The
        # reasoning is at case 12 of the POSIX fixture and in
        # `.agent/rules/distributed-compilation.md`; what matters here is that
        # every property this suite normally checks held while it was broken.
        #
        # `$deadCachePort` comes out of the port block and is never bound, so the
        # connect is REFUSED rather than black-holed: what is under test is the
        # control flow after a transport failure, not how long one takes to notice.
        $deadCachePort = $BasePort + 8
        $deadRoot = Join-Path $scratch "deadcache-proj"
        $deadSrc  = New-Source $deadRoot "$cc-dist-case-deadcache"
        $deadRef  = Join-Path $deadRoot "build\reference.obj"
        $deadObj  = Join-Path $deadRoot "build\u.obj"
        # Compiled to the launcher's own output path and moved aside, as the cases
        # above do: `cl` records that path inside the object.
        & $cc /nologo /c "/Fo$deadObj" $deadSrc | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the case 5 reference compile failed" }
        Move-Item -LiteralPath $deadObj -Destination $deadRef -Force

        $r = Invoke-Dispatching $cc $deadRoot $deadObj "127.0.0.1:$dispatchPort" $deadCachePort
        if ($r.code -ne 0) { Write-Host $r.stderr; throw "the build did not survive an unreachable cache" }
        if ($r.stderr -notmatch "DISPATCHED to ") {
            Write-Host $r.stderr
            Write-Host "--- worker log ---"
            Write-Host (Read-LiveText $workerLog)
            throw "an unreachable cache stopped the compile from being dispatched"
        }
        # The cache failure is still SAID, and said as a cache failure. Reaching
        # the dispatch path must not turn an unreachable daemon into something an
        # operator cannot see -- `--show-stats` ranks this reason.
        if ($r.stderr -notmatch [regex]::Escape("cache unavailable (fetch exchange failed)")) {
            Write-Host $r.stderr
            throw "an unreachable cache was not reported as one"
        }
        # And not as a miss, which would clear that reason and make a broken cache
        # read as a cold one.
        if ($r.stderr -match "fastcache-cc: MISS") {
            Write-Host $r.stderr
            throw "a cache that never answered was traced as a miss"
        }
        # Nothing is pushed at a daemon that did not answer the fetch: before the
        # fix a failed fetch returned, so nothing was ever offered to a daemon that
        # had just failed to answer, and carrying on had to leave that true.
        if ($r.stderr -match "STORED key=") {
            Write-Host $r.stderr
            throw "an object was offered to a cache that never answered"
        }
        if (-not (Test-EquivalentObject $deadRef $deadObj $rules)) {
            throw "the object dispatched around an unreachable cache is wrong"
        }
        Write-Host "   the fleet compiled it with the cache unreachable, and said so"

        Stop-Spawned
    }

    if (-not $ranAnyCompiler) {
        Write-Host "no usable MSVC-family compiler here; skipping"
        exit $SKIP
    }
    Write-Host ""
    Write-Host "dist-compile E2E PASSED"
} catch {
    Write-Host "dist-compile E2E FAILED: $_"
    $exit = 1
} finally {
    Stop-Spawned
}

exit $exit
