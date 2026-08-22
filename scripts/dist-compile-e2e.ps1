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
# is `/E` for MSVC and `preprocessedInputFlags` is EMPTY -- `/E` emits standard
# `#line` that cl accepts in an ordinary source file, so there is nothing to tell
# it, and the `-x c++-cpp-output` spelling has no MSVC equivalent.
#
# Both of those are assertions about a driver that had never been exercised on
# this path. If either is wrong, distribution silently never works on Windows:
# every dispatched translation unit fails and is retried locally, so the build is
# correct, green, and never once helped -- which is exactly the failure the GNU
# side of this already hit, twice.
#
# So the cases here are the ones that would catch that:
#
#   1. Byte-identical    -- a worker's object equals a locally compiled one.
#   2. Still a cache     -- a dispatched result is served from the cache next time.
#   3. Fingerprint       -- a worker for another toolchain is never chosen.
#
# Usage:
#   dist-compile-e2e.ps1 -Fastcached <path> -Node <path> -Launcher <path>
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime prerequisite
# was missing (skip).

param(
    [string]$Fastcached = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Node       = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcache-compile-node.exe",
    [string]$Launcher   = "$PSScriptRoot/../out/build/clangcl-debug/target/fastcache-cc.exe",
    # Fixed rather than probed. The POSIX fixture allocates because it needs four
    # ports and shares a runner with several other socket-using tests; this needs
    # three and is RUN_SERIAL, and a fixed base keeps the failure mode ("something
    # else holds 21730") legible instead of intermittent.
    [int]$BasePort      = 21730
)

$ErrorActionPreference = "Stop"
$SKIP = 77
$exit = 0
$ranAnyCompiler = $false

if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Node))       { Write-Host "fastcache-compile-node not found: $Node; skipping"; exit $SKIP }
if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }

# Start-Process resolves a relative -FilePath against the PROCESS working
# directory rather than PowerShell's, so a caller passing "out/build/..." would
# get a spurious "file not found".
$Fastcached = (Resolve-Path $Fastcached).Path
$Node       = (Resolve-Path $Node).Path
$Launcher   = (Resolve-Path $Launcher).Path

# Scratch beside the build tree, not under %TEMP%, for the reason
# run-launcher-e2e.ps1 records at length: on a GitHub runner %TEMP% is an 8.3
# short name, the two drivers disagree about it, and every root test in the
# launcher is a string prefix comparison. The reconciliation added for issue #66
# handles that now, but a fixture whose roots are ambiguous is testing the
# reconciliation as well as its own property.
$scratch = Join-Path (Split-Path (Split-Path $Launcher -Parent) -Parent) "dist-e2e"

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
    # Written before this ever fired, because every other diagnostic on this branch
    # was written after it cost a round trip.
    $sa = (Get-Item -LiteralPath $a).Length
    $sb = (Get-Item -LiteralPath $b).Length
    Write-Host "  reference: $sa bytes, $ha"
    Write-Host "  produced:  $sb bytes, $hb"
    if ($sa -ne $sb) { return $false }

    Write-Host "  same size, different content -- something environment-specific is embedded"

    # WHERE they differ, which is what actually identifies the culprit and can be
    # read from a CI log without a Windows machine to hand. A COFF header is
    # Machine(2) NumberOfSections(2) TimeDateStamp(4) ... so bytes 4-7 are the
    # timestamp: if the differing offsets are exactly those, the driver simply
    # stamps the clock into every object and byte-identity is unachievable rather
    # than violated. Offsets scattered through the file mean something else --
    # a path, a symbol name -- is leaking.
    $ba = [System.IO.File]::ReadAllBytes($a)
    $bb = [System.IO.File]::ReadAllBytes($b)
    $diffs = [System.Collections.Generic.List[int]]::new()
    for ($i = 0; $i -lt $ba.Length -and $diffs.Count -lt 64; $i++) {
        if ($ba[$i] -ne $bb[$i]) { [void]$diffs.Add($i) }
    }
    $total = 0
    for ($i = 0; $i -lt $ba.Length; $i++) { if ($ba[$i] -ne $bb[$i]) { $total++ } }
    Write-Host ("  {0} differing byte(s); first offsets: {1}" -f $total, (($diffs | Select-Object -First 24) -join ', '))
    if ($diffs.Count -gt 0 -and $diffs[$diffs.Count - 1] -lt 8) {
        Write-Host "  ALL differences are inside the COFF header's first 8 bytes -- that is TimeDateStamp."
        Write-Host "  This driver stamps the clock into every object: byte-identity is unachievable,"
        Write-Host "  and the assertion is what needs to change (see /Brepro)."
    }
    return $false
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
                            [string]$scheduler, [int]$cache) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$cache"
    $env:FASTCACHE_SOURCE_DIR = $root
    $env:FASTCACHE_BINARY_DIR = (Join-Path $root "build")
    $env:FASTCACHE_VERBOSE    = "1"
    if ($scheduler) { $env:FASTCACHE_SCHEDULER = $scheduler }
    else            { Remove-Item -Path "env:FASTCACHE_SCHEDULER" -ErrorAction SilentlyContinue }

    $source  = Join-Path $root "u.cpp"
    $errFile = New-TemporaryFile
    $p = Start-Process -FilePath $Launcher `
        -ArgumentList (ConvertTo-QuotedArgs @($compiler, "/nologo", "/c", "/Fo$obj", $source)) `
        -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stderr = $err }
}

try {
    foreach ($cc in @("cl", "clang-cl")) {
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
            Write-Host "skip $cc (not on PATH)"
            continue
        }

        if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
        New-Item -ItemType Directory -Force -Path $scratch | Out-Null

        if (-not (Test-CompilerWorks $cc $scratch)) {
            Write-Host "skip $cc (on PATH but cannot compile here)"
            continue
        }
        $ranAnyCompiler = $true
        Write-Host "== driver: $cc"

        # Statistics are per-user state; keep this run out of the developer's log.
        $env:LOCALAPPDATA = Join-Path $scratch "state"
        New-Item -ItemType Directory -Force -Path $env:LOCALAPPDATA | Out-Null

        $daemonLog = Join-Path $scratch "daemon.log"
        $daemon = Start-Background $Fastcached @(
            "--listen=127.0.0.1:$cachePort", "--listen-dispatch=127.0.0.1:$dispatchPort",
            "--storage-max-value=64M", "--log-level=info") $daemonLog
        $procs += $daemon
        Wait-ForPort $cachePort    $daemon "daemon" $daemonLog
        Wait-ForPort $dispatchPort $daemon "daemon (dispatch listener)" $daemonLog

        # Asked of the launcher rather than derived here. The fingerprint is a
        # digest over the compiler's whole include tree; a fixture that recomputed
        # it would assert its own reimplementation, and if the two disagreed every
        # case would degrade to a local compile and still exit 0 -- passing while
        # testing nothing.
        $ccPath = (Get-Command $cc).Source
        $fingerprint = (& $Launcher --print-toolchain-fingerprint $ccPath) | Select-Object -First 1
        if (-not $fingerprint) { throw "the launcher reported no toolchain fingerprint for $cc" }

        $workerLog = Join-Path $scratch "worker.log"
        $worker = Start-Background $Node @(
            "--scheduler=127.0.0.1:$dispatchPort", "--bind=127.0.0.1", "--port=$workerPort",
            "--advertise=127.0.0.1:$workerPort", "--toolchain=$ccPath", "--slots=2",
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

        # --- 1 + 2: byte-identical, then served from the cache ---------------
        $root = Join-Path $scratch "proj"
        $src  = New-Source $root "$cc-dist-case-one"
        $refObj = Join-Path $root "build\reference.obj"
        $obj    = Join-Path $root "build\u.obj"

        & $cc /nologo /c "/Fo$refObj" $src | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the reference compile failed" }

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
        # `/E`-preprocessed text must equal one this machine compiled directly.
        if (-not (Test-SameBytes $refObj $obj)) {
            # THE CONTROL. The reference above compiles the ORIGINAL source; the
            # worker compiles `/E`-preprocessed text. Those are different inputs,
            # so a difference between them does not yet say whether distribution
            # is at fault -- it might be inherent to compiling preprocessed text
            # on this driver. GNU produces identical objects either way, which is
            # why the POSIX fixture never had to ask.
            #
            # So ask directly: preprocess and compile locally, the same two steps
            # the worker performs, and compare THAT. Identical means the worker is
            # doing exactly what a local preprocessed compile does and the
            # difference is the input, not the machine. Still different means the
            # worker's environment is leaking into the object, which is the
            # soundness problem this assertion exists to catch.
            Write-Host "--- control: preprocess and compile locally, as the worker does ---"
            $ctlDir = Join-Path $scratch "control"
            New-Item -ItemType Directory -Force -Path $ctlDir | Out-Null
            # Named `tu` to match the worker's own scratch source, so a filename
            # embedded in the object cannot by itself account for a difference.
            $ctlSrc = Join-Path $ctlDir "tu.cpp"
            $ctlObj = Join-Path $ctlDir "tu.o"
            & $cc /nologo /E $src 2>$null | Set-Content -Encoding utf8 $ctlSrc
            if ($LASTEXITCODE -eq 0) {
                & $cc /nologo -c $ctlSrc "/Fo$ctlObj" 2>&1 | Out-Null
                if (Test-Path $ctlObj) {
                    if (Test-SameBytes $ctlObj $obj) {
                        Write-Host "  control MATCHES the worker: the difference is preprocessed-vs-original input,"
                        Write-Host "  not the worker's environment."
                    } else {
                        Write-Host "  control DIFFERS from the worker too."
                        # The control that decides whether this is a LEAK or simply
                        # not achievable. Compile the identical input twice, in the
                        # same directory, seconds apart: if THOSE differ, the driver
                        # does not produce reproducible objects at all -- MSVC stamps
                        # a TimeDateStamp into the COFF header unless /Brepro is
                        # given -- and byte-identity is the wrong assertion for this
                        # platform rather than a violated one.
                        Write-Host "--- control 2: is this driver even reproducible? ---"
                        $ctlObj2 = Join-Path $ctlDir "tu2.o"
                        & $cc /nologo -c $ctlSrc "/Fo$ctlObj2" 2>&1 | Out-Null
                        if (Test-Path $ctlObj2) {
                            if (Test-SameBytes $ctlObj $ctlObj2) {
                                Write-Host "  two identical local compiles MATCH: the driver is reproducible,"
                                Write-Host "  so the worker really is leaking its environment into the object."
                            } else {
                                Write-Host "  two identical local compiles DIFFER: this driver does not produce"
                                Write-Host "  reproducible objects, so byte-identity is unachievable here and the"
                                Write-Host "  assertion -- not the product -- is what needs to change."
                            }
                        } else {
                            Write-Host "  second control compile produced no object; inconclusive"
                        }
                    }
                } else {
                    Write-Host "  control compile produced no object; inconclusive"
                }
            } else {
                Write-Host "  control preprocess failed; inconclusive"
            }
            throw "the worker's object differs from the locally compiled one"
        }
        Write-Host "   byte-identical to the local object"

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

        # --- 3: a worker for another toolchain is never chosen ---------------
        # Its own daemon and its own worker, so the mismatched worker is the ONLY
        # one registered. Reusing the fleet above would leave a matching worker
        # available and the case would pass without testing anything.
        $isoCache    = $BasePort + 3
        $isoDispatch = $BasePort + 4
        $isoWorker   = $BasePort + 5

        $isoDaemonLog = Join-Path $scratch "iso-daemon.log"
        $isoDaemon = Start-Background $Fastcached @(
            "--listen=127.0.0.1:$isoCache", "--listen-dispatch=127.0.0.1:$isoDispatch",
            "--log-level=info") $isoDaemonLog
        $procs += $isoDaemon
        Wait-ForPort $isoDispatch $isoDaemon "isolation daemon" $isoDaemonLog

        $isoWorkerLog = Join-Path $scratch "iso-worker.log"
        $isoNode = Start-Background $Node @(
            "--scheduler=127.0.0.1:$isoDispatch", "--bind=127.0.0.1", "--port=$isoWorker",
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
        & $cc /nologo /c "/Fo$isoRef" $isoSrc | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "the case 3 reference compile failed" }

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
        if (-not (Test-SameBytes $isoRef $isoObj)) {
            throw "the locally compiled fallback object is wrong"
        }
        Write-Host "   a mismatched worker was refused, and the build compiled locally"

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
