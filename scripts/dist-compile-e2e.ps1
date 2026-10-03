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
#  3b. Root-bound        -- a dispatched object naming its checkout (the builtin
#                           `source_location` is made of) is not served into a second
#                           checkout, and is served back to the first.
#  3c. One spelling      -- a header one dispatched compile reaches through two include
#                           chains is ONE /showIncludes note, with no `..` in any note.
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
    # Asked for the scheduler's own record of a worker when a dispatch comes back
    # withdrawn; see `Assert-NotWithdrawn`. Without it a withdrawal still fails, and
    # cannot say which limit took the slots.
    [string]$Cli        = "",
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

# Before the skips and the self-test modes, so a machine that skips this fixture still
# judges it: every launcher it runs must sit inside `Use-E2ELauncherState`, or it would
# read or delete the caller's statistics. The why is written in the module, once.
Import-Module (Join-Path $PSScriptRoot "lib/E2EEnvironment.psm1") -Force
try { Assert-E2ELauncherFixture -Fixture $PSCommandPath }
catch { Write-Host "dist-compile E2E FAILED: $($_.Exception.Message)"; exit 1 }

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

# Every node started below is SOLITARY: a fleet of its own, serving the scheduler its own
# worker registers with, so one process leases, verifies and compiles. A second node cannot
# join a first over loopback, which is how these nodes bind -- a fleet is never offered at an
# address only this machine reaches -- so what needs two machines is asserted in process;
# `dist-compile-e2e.sh` names where. Each runs consensus on a loopback port of its own (`$schedRaftPort`, `$isoRaftPort`)
# and no discovery (`--discovery=`), which is on by default and whose beacon would reach every
# other fixture on this machine.
#
# Every node started below turns its own cache tier OFF. `--listen-node` defaults
# to 0.0.0.0:6674 -- 6674 being where `fastcache-cc` looks -- which is right for the one node
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
#
# The BASE of every run. Each run claims a root of its own under it
# (`New-E2ERunRoot`, never reused), and each driver gets a directory of its own under
# that and a port block of its own (see the driver loop), because a process is not
# known to be gone when whatever follows it starts: `Stop-Spawned` bounds its wait
# and a killed process can outlive the bound. Between RUNS the same thing happened:
# with one root cleared at each start, an immediate rerun died in 0 s on
# "raft-log ... being used by another process" while the holders' PIDs sat in the
# previous run's log. So old roots are swept best-effort, and one that will not go
# is reported with the processes whose command line names it. With one directory shared between the
# drivers, the second one's `Remove-Item` met the first one's isolation worker
# still holding `iso-worker.log` and failed the run with "being used by another
# process" -- a teardown overlap reported as a fault of the case that ran next.
# Not a LOG name problem: with every kill made late, the same removal met the
# isolation scheduler's `raft-log` first, and a state directory has no per-driver
# name to give it. Waiting longer only moves that line; sharing nothing removes it.
#
# Nor is it only this run's processes: Defender opens a file to scan it when its last
# writer closes it, and a delete meeting that handle fails with a sharing violation.
# Measured: `iso-scheduler.log` refused its delete with the Restart Manager naming
# `WinDefend` as the only holder, and the run failed as "The process cannot access the
# file" with every case green.
$scratchBase = Join-Path (Split-Path (Split-Path $Launcher -Parent) -Parent) "dist-e2e"

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

# What the caller pinned, if anything. The block itself is drawn per DRIVER, inside
# the driver loop, for the reason `$scratchBase` gives: a process of the previous
# driver that outlived its teardown bound still holds its ports, and a fresh draw
# steps around it where a shared block would fail the next bind. A pinned block is
# shared by every driver, because pinning one is asking for exactly that.
#
# Not probed under `-SelfTest`, which never reaches the driver loop and opens no
# port of its own -- which is precisely why CMake keeps it in the DEFAULT ctest set
# rather than labelling it `smoke`. Probing there would give that case connect
# attempts it has no use for, and a way to fail ("could not find 9 consecutive free
# ports") on a machine whose ports are none of its business.
$PinnedBasePort = $BasePort

$procs = @()
# What each spawned process was started AS, keyed by PID, so a teardown that finds
# one still running can say which one it is rather than only that one is.
$spawnedCommandLines = @{}

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
    $proc = if ($IsWindows) { Start-Process @common -WindowStyle Hidden } else { Start-Process @common }
    $script:spawnedCommandLines[$proc.Id] = (@($path) + @($common.ArgumentList)) -join ' '
    return $proc
}

# The teardown DECISION -- kill, confirm each process EXITED within one shared
# bound, name what did not -- is shared with node-scratch-isolation-e2e.ps1, and
# its cases run from `-SelfTest` below.
Import-Module (Join-Path $PSScriptRoot "lib/E2EProcesses.psm1") -Force

# How long the killed processes are given, together, to be gone.
#
# A stall bound rather than an estimate: a kill is not instant, and on a
# saturated host it was measured taking 2.2 s with one busy thread per core and
# over 5 s with three -- where the 5 s this used to allow ran out, silently, and
# the run carried on beside a process still holding its port and its log. One
# that is not gone in a minute is wedged, and is reported as such.
$KillWaitMilliseconds = 60000

function Stop-Spawned {
    # Every spawned process, on every exit path. One left holding a port makes the
    # NEXT run fail at startup for a reason unrelated to what actually broke -- so
    # one that outlives the bound is NAMED, rather than returned from in silence.
    #
    # Returns one line per process NOT gone within `$KillWaitMilliseconds`, and the
    # caller prints them at once and decides what they mean. Between drivers they are
    # FATAL when `-BasePort` is pinned, since every driver then shares one port block and
    # the next pass would start beside them; with the default, each driver has a port
    # block and a directory of its own, so they are counted and fail the exit status in
    # the `finally`. In the `finally` they are lines, since an exception there would
    # replace the real diagnostic with one about tearing down.
    $survivors = @(Stop-E2EProcesses $script:procs $script:spawnedCommandLines -BoundMilliseconds $KillWaitMilliseconds)
    $script:procs = @()
    return $survivors
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

# Every grant this worker runs is CHECKED: the worker's own startup line says which lease check it
# built (`MakeWorkerLeaseValidator`), and a worker that fell back to the one verifying nothing would
# pass every case below -- its compiles still run. So the line is WAITED for -- the worker tier is
# built after the scheduler that logs `scheduling for the fleet` -- and its opposite asserted absent.
function Assert-ChecksLeases([string]$path, [string]$what) {
    $text = Wait-ForLine $path ([regex]::Escape("verifying lease signatures against the roster this node applies")) 60 $what
    if ($text -match [regex]::Escape("compiling WITHOUT verifying")) {
        Write-Host $text
        throw "$what compiles WITHOUT verifying lease signatures"
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

# The readiness markers, as a TABLE (#1213).
#
# Two markers, six waits. A row rather than a string at each site is the shape
# #644 landed on the POSIX side, and the reason there was that five copies of one
# wait had each drifted a phrase of their own.
#
# `Core/ReadinessMarker.hpp` is where these literals are DEFINED, and its own doc
# names this file's language as one of the waiters. Keep the bytes: nothing here
# is recompiled by that build, so the text is a published interface. What the
# markers MEAN is written in that table's `meaning` column and is deliberately not
# paraphrased here -- `compile node ready` is `ReadinessFact::Serving`, which has
# no `Bound` enumerator on purpose.
$script:ReadinessMarkerText = @{
    Daemon = 'ready, accepting connections'
    Node   = 'compile node ready'
}

# Bind, THEN the marker, on ONE budget.
#
# ## What was wrong with waiting on the port
#
# A socket that is BOUND is not one that is ACCEPTING, and neither is one that is
# SERVING. Every marker in the table above is emitted strictly after the last
# acceptor arms, precisely because the bind is reachable while nothing will
# answer. Six sites here waited on the connect alone, so #634's node fix and
# #644's daemon fix reached the POSIX fixture and stopped at the language
# boundary. What it produces is the race #634 measured as case 8 -- a node
# compiled against immediately after its bind -- presenting on Windows as *the
# compile was not dispatched to a worker*, which sends the reader to the
# scheduler.
#
# ## Why the bind leg is KEPT rather than replaced
#
# It names a class the marker cannot: a process that exits before it ever
# listens, with its own log printed. Dropping it would turn every such startup
# refusal into one undifferentiated readiness timeout.
#
# ## The two refusals, and why they are refusals rather than waits
#
# A role with no row and an empty log path are both PROGRAMMER errors, and both
# fail toward silence if they are allowed through: an absent marker would make
# `Contains('')` true on the first poll, so the wait would return instantly
# having established nothing -- the PowerShell shape of the `grep -q "$marker" -`
# hazard `_e2e_wait_ready` refuses by name on the POSIX side.
#
# @param role   a key of $ReadinessMarkerText
# @param port   the port whose bind is the first leg
# @param proc   the process, so an early exit is reported as one
# @param what   the participant's name, for every message
# @param log    the file the marker is read out of; required
# @param seconds the budget SHARED by both legs
function Wait-ForReady([string]$role, [int]$port, [System.Diagnostics.Process]$proc,
                       [string]$what, [string]$log, [int]$seconds = 20) {
    if (-not $script:ReadinessMarkerText.ContainsKey($role)) {
        throw "Wait-ForReady: no readiness marker for role '$role' (the table has: $(($script:ReadinessMarkerText.Keys | Sort-Object) -join ', '))"
    }
    $marker = $script:ReadinessMarkerText[$role]
    if (-not $log) {
        throw "Wait-ForReady: $what was given no log, and the marker is only readable out of one"
    }

    $polls = [Math]::Max(1, $seconds * 5)
    $spent = 0

    $bound = $false
    while ($spent -lt $polls -and -not $bound) {
        if ($proc -and $proc.HasExited) {
            Write-Host (Read-LiveText $log)
            throw "$what exited before listening (exit $($proc.ExitCode))"
        }
        $client = New-Object System.Net.Sockets.TcpClient
        try {
            $client.Connect("127.0.0.1", $port)
            $client.Close()
            $bound = $true
        } catch {
            Start-Sleep -Milliseconds 200
            $spent++
        } finally {
            $client.Dispose()
        }
    }
    if (-not $bound) {
        Write-Host (Read-LiveText $log)
        throw "$what never listened on port $port"
    }

    # The readiness leg runs on what the bind LEFT of the same budget, and the
    # message says so -- otherwise a slow start is reported against a number no
    # caller configured.
    while ($spent -lt $polls) {
        if ($proc -and $proc.HasExited) {
            Write-Host (Read-LiveText $log)
            throw "$what exited before it was ready (exit $($proc.ExitCode))"
        }
        $text = Read-LiveText $log
        # A literal substring, never `-match`: the marker is data, and a regex
        # would make its punctuation meaningful.
        if ($text -and $text.Contains($marker)) { return }
        Start-Sleep -Milliseconds 200
        $spent++
    }
    Write-Host (Read-LiveText $log)
    # It must NOT claim a bind failure. The port is open and answering -- that is
    # the whole reason this wait exists -- and a message pointing at the port
    # sends the reader to check something that is working (#652).
    throw "$what never reported readiness within a ${seconds}s budget shared with the bind (marker: '$marker'). Its port IS bound and answering, so an open port is not evidence this succeeded."
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
                            [string]$scheduler, [int]$cache, [string]$sourceName = "u.cpp",
                            [string[]]$extra = @()) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$cache"
    $env:FASTCACHE_SOURCE_DIR = $root
    $env:FASTCACHE_BINARY_DIR = (Join-Path $root "build")
    $env:FASTCACHE_VERBOSE    = "1"
    if ($scheduler) { $env:FASTCACHE_SCHEDULER = $scheduler }
    else            { Remove-Item -Path "env:FASTCACHE_SCHEDULER" -ErrorAction SilentlyContinue }

    $source  = Join-Path $root $sourceName
    $outFile = New-TemporaryFile
    $errFile = New-TemporaryFile
    $p = Use-E2ELauncherState $launcherState {
        Start-Process -FilePath $Launcher `
            -ArgumentList (ConvertTo-QuotedArgs (@($compiler, "/nologo", "/c", "/Fo$obj") + $extra + @($source))) `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    }
    $out = Get-Content -Raw $outFile -ErrorAction SilentlyContinue
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stdout = [string]$out; stderr = $err }
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

# ---- a worker that withdrew its slots --------------------------------------
#
# The worker every dispatching case uses is offered one slot above this host's
# core count (see `$workerSlots`), so CPU used outside this fleet cannot withdraw
# it: a `rejected (withdrawn)` is a FAILURE, never a skip, and the useful thing to
# say is WHICH limit took the slots -- memory and scratch still can, and should, on
# a starved host. Load-driven withdrawal itself is the product's behaviour and is
# covered where it lives: `NodePolicy_test.cpp`'s `SlotCeilingsFor` cases, and the
# `Withdrawn` refusal in `SchedulerProtocol_test.cpp` and `WorkerRegistry_test.cpp`.

# Whether a launcher's output is the scheduler refusing a lease because every
# matching worker withdrew. Pure, so the self-test drives it without a process.
# @param stderr The launcher's stderr for a compile that was not dispatched.
# @return True for a withdrawal.
function Test-WithdrawnRefusal([string]$stderr) {
    return $stderr -match "not dispatched \(rejected \(withdrawn\)"
}

# The scheduler's `limited-by` for one worker, out of `fleet workers` as JSON.
# @param json        What `fastcache-cli --format=json fleet workers` printed.
# @param endpoint    The worker's advertised endpoint.
# @param fingerprint The toolchain the lease asked for.
# @return The `limited-by` text, or $null when no such worker is in the record.
function Get-WorkerLimit([string]$json, [string]$endpoint, [string]$fingerprint) {
    try { $rows = @($json | ConvertFrom-Json) } catch { return $null }
    foreach ($row in $rows) {
        if ($row.endpoint -eq $endpoint -and $row.toolchain -eq $fingerprint) { return $row.'limited-by' }
    }
    return $null
}

# Read the scheduler's record of one worker, or $null when it cannot be read.
#
# Bounded, because it is asked on a path that is already failing and an unbounded
# ask there turns a named refusal into a hang: the client's own ceilings are 5 s to
# connect and 10 s per read and write, so `$CliReadSeconds` is their sum. An expiry
# is SAID, naming what was waited for, and reads as an absent record.
$CliReadSeconds = 15
function Read-WorkerLimit([string]$scheduler, [string]$endpoint, [string]$fingerprint) {
    if (-not $Cli -or -not (Test-Path $Cli)) { return $null }
    $out = Join-Path ([IO.Path]::GetTempPath()) ("dist-e2e-fleet-" + [Guid]::NewGuid().ToString("N") + ".json")
    $asked = Start-Process -FilePath $Cli -PassThru -NoNewWindow `
        -ArgumentList (ConvertTo-QuotedArgs @("--addr=$scheduler", "--format=json", "fleet", "workers")) `
        -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    try {
        if (-not $asked.WaitForExit($CliReadSeconds * 1000)) {
            try { $asked.Kill() } catch { $null = $_ }
            Write-Host "the scheduler's record was not read: fastcache-cli fleet workers against $scheduler did not answer within $CliReadSeconds s"
            return $null
        }
        if ($asked.ExitCode -ne 0) { return $null }
        return Get-WorkerLimit (Get-Content -Raw -LiteralPath $out) $endpoint $fingerprint
    } finally {
        Remove-Item -LiteralPath $out, "$out.err" -ErrorAction SilentlyContinue
    }
}

# Called where a dispatch the case needed did not happen. A withdrawal THROWS,
# naming the limit the scheduler's record gives -- the worker is sized so CPU used
# outside this fleet cannot cause one (`$workerSlots`), so whatever did is the
# finding. Anything else returns, and the caller's own failure stands.
function Assert-NotWithdrawn($result, [string]$scheduler, [string]$workerEndpoint, [string]$fingerprint) {
    if (-not (Test-WithdrawnRefusal $result.stderr)) { return }
    $limit = Read-WorkerLimit $scheduler $workerEndpoint $fingerprint
    $limitText = if ($null -eq $limit) { "unreadable" } else { $limit }
    Write-Host $result.stderr
    throw "the worker withdrew its slots (the scheduler's record says it is limited by '$limitText'), which it is sized never to do for CPU used outside this fleet -- see `$workerSlots"
}

function Invoke-SelfTest {
    $failures = 0
    function Assert-That([bool]$condition, [string]$what) {
        $script:selfTestCases++
        if ($condition) { Write-Host "   ok   $what" }
        else { Write-Host "   FAIL $what"; $script:selfTestFailures++ }
    }
    $script:selfTestFailures = 0
    $script:selfTestCases = 0

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

        # ---- Test-WithdrawnRefusal ---------------------------------------
        $withdrawn = "fastcache-cc: not dispatched (rejected (withdrawn): every matching worker has withdrawn its capacity); compiling locally"
        $noWorker  = "fastcache-cc: not dispatched (rejected (no-worker): no worker serves this toolchain); compiling locally"
        Assert-That (Test-WithdrawnRefusal $withdrawn) "a withdrawn refusal is recognised"
        Assert-That (-not (Test-WithdrawnRefusal $noWorker)) "a different refusal is not a withdrawal"
        Assert-That (-not (Test-WithdrawnRefusal "")) "nor is a compile that said nothing"

        # The scheduler's record as `fastcache-cli --format=json fleet workers`
        # prints it -- captured from a real run, with a second row added -- so
        # the reader is asked which worker it is looking at, not just whether a
        # `limited-by` exists.
        $fleet = '[{"id":"w1","toolchain":"3f5d2a3eca09d20f79b8bf9217019b87","compiler":"cl 19.51.36252","endpoint":"127.0.0.1:25271","slots":"32","in-flight":"0","available":"0","limited-by":"external-cpu","heartbeat-age":"2078","registered-age":"2078","last-picked-age":null},' +
                 '{"id":"w2","toolchain":"3f5d2a3eca09d20f79b8bf9217019b87","compiler":"cl 19.51.36252","endpoint":"127.0.0.1:25999","slots":"4","in-flight":"0","available":"4","limited-by":"registered","heartbeat-age":"10","registered-age":"10","last-picked-age":null}]'
        Assert-That ((Get-WorkerLimit $fleet "127.0.0.1:25271" "3f5d2a3eca09d20f79b8bf9217019b87") -eq "external-cpu") "the worker's own row is read"
        Assert-That ((Get-WorkerLimit $fleet "127.0.0.1:25999" "3f5d2a3eca09d20f79b8bf9217019b87") -eq "registered") "and not its neighbour's"
        Assert-That ($null -eq (Get-WorkerLimit $fleet "127.0.0.1:25271" "9e5d3aaa03c5f0c2564bda4c6c68f021")) "another toolchain at that endpoint is not this worker"
        Assert-That ($null -eq (Get-WorkerLimit "not json" "127.0.0.1:25271" "3f5d2a3eca09d20f79b8bf9217019b87")) "an unreadable record is absent, not a limit"

        # ---- the readiness waits (#1213) --------------------------------
        #
        # Driven here rather than through the fixture, for the reason
        # `node-scratch-isolation-e2e-selftest` records: the DECISION is what can
        # be wrong, and a case that needs a daemon, a node and a compiler is
        # reachable only where the whole fixture already runs -- which is the
        # population it is not for.
        #
        # A REAL listener, because the first leg is a real connect. Port 0 lets
        # the kernel choose, so this touches neither `$BasePort` nor the ledger.
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
        $listener.Start()
        try {
            $readyPort = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
            # This process: it has certainly not exited, so the early-exit arm
            # cannot fire and every case below turns on the marker.
            $me = Get-Process -Id $PID

            # What was thrown, so a case can assert WHICH refusal it got. A test
            # that only asserts "it threw" passes when it throws for the wrong
            # reason, and two of the cases below differ only in their message.
            function Get-Refusal([scriptblock]$block) {
                try { & $block; return "" } catch { return $_.Exception.Message }
            }

            $nodeLog = Join-Path $dir "ready-node.log"
            Set-Content -Path $nodeLog -Value "starting`ncompile node ready, serving 1 of 1 toolchain(s)`n"

            # The literals, pinned. They are a published interface for a fixture
            # this build does not recompile, so a reword here is a wire change.
            Assert-That ($script:ReadinessMarkerText.Node -eq 'compile node ready') "the node marker is the published literal"
            Assert-That ($script:ReadinessMarkerText.Daemon -eq 'ready, accepting connections') "the daemon marker is the published literal"

            # The POSITIVE direction first: every refusal below is evidence only
            # if the right marker on a bound port actually returns.
            $m = Get-Refusal { Wait-ForReady Node $readyPort $me "a node whose marker is present" $nodeLog 5 }
            Assert-That ($m -eq "") "the right marker on a bound port returns"

            # THE CASE THIS TICKET IS ABOUT. The port answers throughout, so a
            # wait that merely connects passes here; one pointed at the wrong
            # marker must not.
            $m = Get-Refusal { Wait-ForReady Daemon $readyPort $me "a node waited on with the DAEMON marker" $nodeLog 1 }
            Assert-That ($m -like "*never reported readiness*") "the WRONG marker refuses, though the port answers throughout"
            Assert-That ($m -like "*port IS bound and answering*") "and the refusal says an open port is not evidence"

            $silent = Join-Path $dir "ready-silent.log"
            Set-Content -Path $silent -Value "starting`n"
            $m = Get-Refusal { Wait-ForReady Node $readyPort $me "a node that binds and never reports" $silent 1 }
            Assert-That ($m -like "*never reported readiness*") "a bound process that never reports is a READINESS failure, not a bind one"

            # The two programmer errors, refused rather than waited on. Both fail
            # toward silence if they are let through: an absent row makes the
            # marker empty, and an empty needle is found in everything.
            $m = Get-Refusal { Wait-ForReady Nonesuch $readyPort $me "a role nobody registered" $nodeLog 1 }
            Assert-That ($m -like "*no readiness marker for role 'Nonesuch'*") "an unknown role is refused BY NAME"
            $m = Get-Refusal { Wait-ForReady Node $readyPort $me "a wait with no log" "" 1 }
            Assert-That ($m -like "*given no log*") "an empty log is refused rather than polled"
        } finally {
            $listener.Stop()
        }

        # ---- the teardown's report --------------------------------------
        #
        # The cases live beside the decision, in `lib/E2EProcesses.psm1`; running
        # them here also proves this fixture's import of it works.
        $teardown = Invoke-E2EProcessesSelfTest
        $script:selfTestCases += $teardown.Cases
        $script:selfTestFailures += $teardown.Failures
    } finally {
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    }

    # A self-test that stopped early must not look like one that judged
    # something, so the COUNT is printed on both paths out. It is not written
    # down in a comment anywhere: it moves whenever a case is added, and a
    # restated total is a second thing to be wrong.
    if ($script:selfTestFailures -ne 0) {
        Write-Host "dist-compile-e2e self-test FAILED ($script:selfTestFailures of $script:selfTestCases case(s): object comparison, the withdrawal refusal, the readiness waits, the teardown report and scratch roots)"
        return 1
    }
    Write-Host "dist-compile-e2e self-test PASSED ($script:selfTestCases case(s): object comparison, the withdrawal refusal, the readiness waits, the teardown report and scratch roots)"
    return 0
}

if ($SelfTest) { exit (Invoke-SelfTest) }

# The launcher reads its whole configuration from FASTCACHE_* variables, and the calls
# below set only the ones they mean -- so an inherited FASTCACHE_VERIFY, FASTCACHE_TOKEN
# or FASTCACHE_NO_DIRECT would decide what a case measures. Importing the shared module
# clears every one of them; this fixture draws no port through `E2EPorts.psm1`, which
# would otherwise have imported it.
Import-Module (Join-Path $PSScriptRoot "lib/E2EEnvironment.psm1") -Force


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

$runRoot = $null
# Every process a teardown between drivers could not end, for the verdict in `finally`.
$script:killSurvivors = @()
# Every launcher this fixture runs records into a state directory of the run's own,
# through `Use-E2ELauncherState` in `scripts/lib/E2EEnvironment.psm1`, which also
# refuses this file if any launcher runs outside it. This replaces a LOCALAPPDATA the
# driver loop set process-wide and never put back. Inside the `try` whose `finally`
# removes the state root and reports what the run did to the caller's logs.
$launcherState = $null
try {
    # Old roots first, reported and never fatal; then this run's own: see `$scratchBase`.
    foreach ($line in @(Clear-E2EStaleRoots -Base $scratchBase)) { Write-Host "stale scratch: $line" }
    $runRoot = New-E2ERunRoot -Base $scratchBase
    $scratchRoot = $runRoot.Path
    Write-Host "== scratch root for this run: $scratchRoot"
    $launcherState = Enter-E2ELauncherState -Launcher $Launcher -Fixture $PSCommandPath -RunTrees @($scratchRoot)
    Write-Host "launcher statistics isolated to $($launcherState.Log)"

    foreach ($driver in $Drivers) {
        $cc = $driver.Name
        $rules = $driver.Rules
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
            Write-Host "skip $cc (not on PATH)"
            continue
        }

        # This driver's own directory and its own port block -- nothing the previous
        # driver's processes could still be holding (see `$scratchBase`).
        $scratch = Join-Path $scratchRoot $cc
        New-Item -ItemType Directory -Force -Path $scratch | Out-Null
        $BasePort = if ($PinnedBasePort -ne 0) { $PinnedBasePort } else { Get-FreePortBlock $PortsNeeded }
        $cachePort    = $BasePort
        $dispatchPort = $BasePort + 1
        # Each node's consensus port (#178): a node is a cluster of one, bound to loopback
        # where nothing dials it. +6 and +7 were free since the dedicated compile port went;
        # +2 and +5, which were the separate workers' ports, are drawn and left unused.
        $schedRaftPort = $BasePort + 6
        $isoRaftPort   = $BasePort + 7

        # No key file (#178 PR 6): every node's scheduler signs with the node's own identity
        # key, and its worker CHECKS the signature against the state the node's own consensus
        # applies. No key is typed and nothing is admitted by hand. The shell twin does the same.

        if (-not (Test-CompilerWorks $cc $scratch)) {
            Write-Host "skip $cc (on PATH but cannot compile here)"
            continue
        }
        $ranAnyCompiler = $true
        Write-Host "== driver: $cc (ports $BasePort..$($BasePort + $PortsNeeded - 1), scratch $scratch)"

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
        Wait-ForReady Daemon $cachePort $daemon "daemon" $daemonLog

        # Asked of the launcher rather than derived here. The fingerprint is a
        # digest over the compiler's whole include tree; a fixture that recomputed
        # it would assert its own reimplementation, and if the two disagreed every
        # case would degrade to a local compile and still exit 0 -- passing while
        # testing nothing.
        $ccPath = (Get-Command $cc).Source
        $fingerprint = Use-E2ELauncherState $launcherState { (& $Launcher --print-toolchain-fingerprint $ccPath) | Select-Object -First 1 }
        if (-not $fingerprint) { throw "the launcher reported no toolchain fingerprint for $cc" }

        # ONE slot above the node's own core count, so no amount of CPU used outside
        # this fleet can withdraw the worker these cases dispatch to.
        #
        # `SlotCeilingsFor` charges other work only past the headroom the slots leave,
        # and with the slots above the cores there is no headroom: every external core
        # is charged. There are at most `logicalCores` of them, so the ceiling never
        # falls below `slots - logicalCores`, which is one. With the slots AT the core
        # count it fell to zero on any host with no idle core, and a host running other
        # builds beside this suite is exactly that: the worker withdrew `external-cpu`,
        # and case 5 -- whose subject is an unreachable CACHE -- ended as a skip or a
        # failure about a withdrawal it was never about.
        #
        # Oversubscribing is a supported configuration rather than a trick:
        # `OfferableSlots` takes an operator's `--slots` untouched, precisely so a
        # machine can be offered more jobs than it has cores. The other two ceilings
        # are not covered and should not be: a host with under 1 GiB of memory or
        # 128 MiB of scratch left is starved, not busy, and still withdraws.
        #
        # Counted so it cannot fall SHORT of the node's count, which is
        # `GetSystemInfo`'s processor count. `[Environment]::ProcessorCount` honours
        # this process's affinity mask and so can only be lower; the CIM figure is the
        # whole machine and can only be higher, which is the safe direction. The larger
        # of the two is used.
        #
        # The `--slots=1` workers elsewhere in this file are deliberate and stay:
        # their cases are ABOUT a worker having exactly one.
        $hostCores = [Environment]::ProcessorCount
        $machine = Get-CimInstance Win32_ComputerSystem -ErrorAction SilentlyContinue
        if ($machine -and $machine.NumberOfLogicalProcessors -gt $hostCores) {
            $hostCores = [int]$machine.NumberOfLogicalProcessors
        }
        $workerSlots = $hostCores + 1

        # One compile node: it serves the fleet's scheduler -- a first start is a cluster of
        # one, and its mode serves one -- and its own worker registers with it, so the
        # scheduler that leases this worker is the one in the same process. A serving node
        # is refused `--scheduler`, so there is no second process to point at it.
        # --fleet-open because the policy has to be STATED; the clients here are this
        # machine and admitted either way. The node is the ONLY worker its scheduler has,
        # so "which worker ran this job" is never a race. One port: the scheduler and the
        # compile verbs answer on --listen-node, and --advertise names it.
        $workerLog = Join-Path $scratch "worker.log"
        $workerState = Join-Path $scratch "worker.state"
        $worker = Start-Background $Node @(
            $NoLocalCache, "--fleet-open",
            "--listen-node=127.0.0.1:$dispatchPort", "--advertise=127.0.0.1:$dispatchPort",
            "--listen-raft=127.0.0.1:$schedRaftPort", "--raft-self=127.0.0.1",
            "--cluster-dir=$workerState", "--discovery=",
            "--toolchain=$ccPath", "--slots=$workerSlots",
            "--log-level=debug") $workerLog
        $procs += $worker
        Wait-ForReady Node $dispatchPort $worker "worker" $workerLog
        Wait-ForLine $workerLog "scheduling for the fleet" 60 "worker" | Out-Null
        Assert-ChecksLeases $workerLog "worker"
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
            Assert-NotWithdrawn $r "127.0.0.1:$dispatchPort" "127.0.0.1:$dispatchPort" $fingerprint
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
            Assert-NotWithdrawn $r "127.0.0.1:$dispatchPort" "127.0.0.1:$dispatchPort" $fingerprint
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

        # --- 3b: a dispatched object naming its checkout stays with it -------
        #
        # A worker compiles `/E` text whose line markers name the CLIENT's paths, so
        # the builtin `std::source_location` is made of resolves to the client's
        # checkout on the worker -- and no key sees it, because it is filled in after
        # preprocessing. The dispatched object is stored by the same code as a local
        # one, scanned against the client's roots (apps/fastcache-cc/RootBinding.hpp),
        # so a second checkout must MISS through the first one's marker and dispatch
        # its own, and the first must still HIT its own copy without dispatching.
        # Before root binding the second checkout was served the first one's object.
        $boundRoots = @{}
        foreach ($co in 'checkout-a', 'checkout-b') {
            $broot = Join-Path $scratch "bound\$co"
            New-Item -ItemType Directory -Force -Path (Join-Path $broot "build") | Out-Null
            New-Item -ItemType Directory -Force -Path (Join-Path $broot "inc") | Out-Null
            "#pragma once`ninline int One() { return 1; }" | Set-Content -Encoding utf8 (Join-Path $broot "inc\h1.h")
            ("#include `"inc/h1.h`"`nchar const* Tag() { return `"$cc-dist-case-bound`"; }`n" +
             "char const* Where() { return __builtin_FILE(); }`nint G() { return One(); }") |
                Set-Content -Encoding utf8 (Join-Path $broot "u.cpp")
            $boundRoots[$co] = $broot
        }
        $objA = Join-Path $boundRoots['checkout-a'] "build\u.obj"
        $objB = Join-Path $boundRoots['checkout-b'] "build\u.obj"
        $rA = Invoke-Dispatching $cc $boundRoots['checkout-a'] $objA "127.0.0.1:$dispatchPort" $cachePort
        $rB = Invoke-Dispatching $cc $boundRoots['checkout-b'] $objB "127.0.0.1:$dispatchPort" $cachePort
        Remove-Item -LiteralPath $objA -Force -ErrorAction SilentlyContinue
        $rA2 = Invoke-Dispatching $cc $boundRoots['checkout-a'] $objA "127.0.0.1:$dispatchPort" $cachePort
        $bNamesA = (Test-Path $objB) -and [System.Text.Encoding]::Latin1.GetString(
            [System.IO.File]::ReadAllBytes($objB)).Contains($boundRoots['checkout-a'])
        $okA = $rA.code -eq 0 -and $rA.stderr -match "DISPATCHED to " -and $rA.stderr -match "root-bound object"
        $okB = $rB.code -eq 0 -and $rB.stderr -match "fastcache-cc: MISS key=\S+ \(root-bound:" `
               -and $rB.stderr -match "DISPATCHED to " -and (Test-Path $objB) -and -not $bNamesA
        $okA2 = $rA2.code -eq 0 -and $rA2.stderr -match "fastcache-cc: HIT key=\S+ \(root-bound:" `
                -and $rA2.stderr -notmatch "DISPATCHED to "
        if (-not ($okA -and $okB -and $okA2)) {
            foreach ($leg in @(@{n="a"; r=$rA}, @{n="b"; r=$rB}, @{n="a again"; r=$rA2})) {
                Write-Host "--- $($leg.n) ---"
                Write-Host $leg.r.stderr
            }
            throw "a dispatched root-bound object was not kept with its checkout (a=$okA b=$okB b-names-a=$bNamesA a-again=$okA2)"
        }
        Write-Host "   a dispatched object naming its checkout was kept with it, and served back to it"

        # --- 3c: one header, two include chains, one note (#1593) ------------
        #
        # A worker sees no `#include`, so the client writes a dispatched compile's
        # /showIncludes notes from its probe's dependency list. `cl` reports a header it
        # reaches twice in two spellings -- `<root>\a/x.h` directly, `<root>\b\../a/x.h`
        # through `b/y.h` (measured, VS 18) -- and the renderer's dedup is byte-exact, so
        # before the list was collapsed and spelled one way at the probe boundary this
        # wrote two notes for one file. The header is UNGUARDED on purpose: both drivers
        # skip the note for a guarded header's second inclusion entirely.
        $chainRoot = Join-Path $scratch "chains"
        New-Item -ItemType Directory -Force -Path (Join-Path $chainRoot "build"), (Join-Path $chainRoot "a"),
            (Join-Path $chainRoot "b") | Out-Null
        "extern int xv;" | Set-Content -Encoding utf8 (Join-Path $chainRoot "a\x.h")
        "#include `"../a/x.h`"`ninline int Y() { return 2; }" | Set-Content -Encoding utf8 (Join-Path $chainRoot "b\y.h")
        ("#include `"a/x.h`"`n#include `"b/y.h`"`nchar const* Tag() { return `"$cc-dist-case-chains`"; }`n" +
         "int U() { return Y() + xv; }") | Set-Content -Encoding utf8 (Join-Path $chainRoot "u.cpp")
        $chainObj = Join-Path $chainRoot "build\u.obj"
        $rC = Invoke-Dispatching $cc $chainRoot $chainObj "127.0.0.1:$dispatchPort" $cachePort "u.cpp" @("/showIncludes")
        $notes = @(($rC.stdout -split "`r?`n") | Where-Object { $_ -match '^Note: including file:' })
        $xNotes = @($notes | Where-Object { $_ -match '[\\/]a[\\/]x\.h$' })
        $yNotes = @($notes | Where-Object { $_ -match '[\\/]b[\\/]y\.h$' })
        $dotted = @($notes | Where-Object { $_ -match '(^|[\\/ ])\.\.([\\/]|$)' })
        if (-not ($rC.code -eq 0 -and $rC.stderr -match "DISPATCHED to " -and $xNotes.Count -eq 1 -and $yNotes.Count -eq 1 `
                  -and $dotted.Count -eq 0)) {
            Write-Host "--- stdout ---"; Write-Host $rC.stdout
            Write-Host "--- stderr ---"; Write-Host $rC.stderr
            throw ("a header reached through two include chains was not ONE note (x.h notes=$($xNotes.Count), " +
                   "y.h notes=$($yNotes.Count), notes with '..'=$($dotted.Count))")
        }
        Write-Host "   a header reached through two include chains was one note, with no '..' in any"

        # --- 4: a worker for another toolchain is never chosen ---------------
        # Its own daemon and its own node, so the mismatched worker is the ONLY one its
        # scheduler has. Reusing the node above would leave a matching worker available
        # and the case would pass without testing anything.
        $isoCache    = $BasePort + 3
        $isoDispatch = $BasePort + 4

        $isoDaemonLog = Join-Path $scratch "iso-daemon.log"
        $isoDaemon = Start-Background $Fastcached @(
            "--listen=127.0.0.1:$isoCache",
            "--log-level=info") $isoDaemonLog
        $procs += $isoDaemon
        Wait-ForReady Daemon $isoCache $isoDaemon "isolation daemon" $isoDaemonLog

        # One node, serving a toolchain this client does not use: its own worker is the
        # only one its scheduler has.
        $isoWorkerLog = Join-Path $scratch "iso-worker.log"
        $isoWorkerState = Join-Path $scratch "iso-worker.state"
        $isoNode = Start-Background $Node @(
            $NoLocalCache, "--fleet-open",
            "--listen-node=127.0.0.1:$isoDispatch", "--advertise=127.0.0.1:$isoDispatch",
            "--listen-raft=127.0.0.1:$isoRaftPort", "--raft-self=127.0.0.1",
            "--cluster-dir=$isoWorkerState", "--discovery=",
            "--toolchain=not-the-compiler-this-client-uses=$ccPath", "--slots=2",
            "--log-level=debug") $isoWorkerLog
        $procs += $isoNode
        Wait-ForReady Node $isoDispatch $isoNode "isolation worker" $isoWorkerLog
        Wait-ForLine $isoWorkerLog "scheduling for the fleet" 60 "isolation worker" | Out-Null
        Assert-ChecksLeases $isoWorkerLog "isolation worker"
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
            Assert-NotWithdrawn $r "127.0.0.1:$dispatchPort" "127.0.0.1:$dispatchPort" $fingerprint
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

        # --- no compilation-directory case here, and that is a DECISION ------
        #
        # The POSIX fixture's case 13 asserts that a dispatched object and a local
        # one record the same `DW_AT_comp_dir` (#506). There is deliberately no
        # counterpart on this platform, and the omission is written down rather
        # than left as an absence: absent and skipped are different states, and an
        # unexplained gap is how somebody comes to add a case that cannot pass.
        #
        # Neither COFF driver has a path-map switch that reaches the records in
        # question. `cl` has none at all, and `-ffile-prefix-map` does not remap
        # clang-cl's CodeView `S_OBJNAME` or the `-cc1` line it embeds -- measured
        # under #203, where an object built with it still differed cross-root by
        # the same 23 bytes. So `WorkerPrefixMapRule` refuses an MSVC-family worker
        # outright rather than pretending, and there is no value for a case here to
        # assert. `.agent/rules/compile-cache.md` records that residue as an
        # accepted cost of cross-checkout sharing on this platform.
        Write-Host "== no case 13 here: neither COFF driver has a path-map switch (see #203, #506)"

        # Named WHEN FOUND, so the line precedes whatever the next driver reports. Fatal
        # here only with `-BasePort` pinned: every driver then shares one port block, and
        # the next pass would start beside a process still holding it. With the default
        # the next pass has a directory and a port block of its own (see `$scratchBase`),
        # so a survivor holds nothing it will reach for; it is still not a clean run, so
        # it is counted and the `finally` fails the exit status.
        $found = @(Stop-Spawned)
        foreach ($line in $found) { Write-Host "teardown after ${cc}: $line" }
        $script:killSurvivors += $found
        if ($found.Count -gt 0 -and $PinnedBasePort -ne 0) {
            throw "$($found.Count) process(es) outlived the teardown after $cc, and -BasePort pins every driver to one port block"
        }
    }

    if (-not $ranAnyCompiler) {
        Write-Host "no usable MSVC-family compiler here; skipping"
        exit $SKIP
    }

    # The positive control: this run's compiles recorded in its own log.
    $recorded = Get-E2ELauncherStateRecordCount $launcherState
    if ($recorded -lt 1) { throw "the launcher recorded nothing in this run's state log ($($launcherState.Log))" }
    Write-Host "this run's compiles were recorded in its own state log: $recorded record(s)"

    Write-Host ""
    Write-Host "dist-compile E2E PASSED"
} catch {
    Write-Host "dist-compile E2E FAILED: $_"
    $exit = 1
} finally {
    # Those found between drivers were printed when found; only this teardown's are new.
    $final = @(Stop-Spawned)
    foreach ($line in $final) { Write-Host "teardown: $line" }
    # What the run did to the caller's logs, reported on EVERY way out -- a failure path is
    # where a leak shows -- and a failure whatever else happened.
    $callerDamage = @(Exit-E2ELauncherState $launcherState)
    foreach ($line in $callerDamage) { Write-Host "dist-compile E2E FAILED: $line" }
    $survivors = @($script:killSurvivors) + $final
    # Released LAST, once nothing this run started should still be writing under it.
    if ($null -ne $runRoot) { $runRoot.Claim.Dispose() }
    if ($callerDamage.Count -gt 0) { exit 1 }
    # And not a clean exit. A run that is already failing keeps its failure, which is
    # the diagnostic that matters; a pass -- or a SKIP, whose `exit` is unwinding
    # through here -- leaving a process it could not kill is not one, so the `exit`
    # here overrides it.
    if ($survivors.Count -gt 0 -and $exit -ne 1) { exit 1 }
}

exit $exit
