# Two compile-node PROCESSES on one host must not share a scratch root (#279).
#
# The acceptance criterion from the ticket, as a test: two nodes, same toolchain,
# concurrent compiles of DIFFERENT translation units, each getting its own object.
#
# Why an end-to-end fixture and not only the unit tests beside `ScratchClaim.cpp`:
# those prove the claim mechanism, this proves the WIRING -- that the node actually
# claims a root before it compiles anything. The defect was never in the mechanism;
# there was no mechanism, and every unit test in the tree passed.
#
# Two phases, and the second is the control that makes the first mean something:
#
#   1. Both nodes inherit one TEMP, so both derive the same scratch base.
#   2. One node is given a TEMP of its own, so the bases differ and nothing else
#      about the run changes.
#
# Before the fix, phase 1 lost a compile and phase 2 did not, which is what isolated
# the scratch root as the cause. Both must now be clean.
#
# The overlap is asserted rather than assumed: without it each node takes its root
# in sequence, nothing is contended, and the run reports green having tested nothing.

[CmdletBinding()]
param(
    [string]$Fastcached,
    [string]$Node,
    [string]$Launcher,
    [string]$Compiler
)

$ErrorActionPreference = "Stop"
$SkipExit = 77

function Skip([string]$why) {
    Write-Host "node-scratch-isolation-e2e: $why -- skipping"
    exit $SkipExit
}

foreach ($pair in @(@{p=$Fastcached;n="-Fastcached"}, @{p=$Node;n="-Node"}, @{p=$Launcher;n="-Launcher"})) {
    if (-not $pair.p -or -not (Test-Path $pair.p)) { Skip "$($pair.n) was not given a built binary" }
}
if (-not $Compiler) { $Compiler = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source }
if (-not $Compiler -or -not (Test-Path $Compiler)) { Skip "no MSVC cl.exe on PATH" }

# Ports allocated per RUN, from BELOW the kernel's ephemeral range, and remembered.
# A connect probe cannot see a port already held as an outbound connection's local
# endpoint, so a port handed out once is never handed out again in this process.
$script:TakenPorts = @{}
function Get-FreePort {
    for ($attempt = 0; $attempt -lt 200; $attempt++) {
        $candidate = Get-Random -Minimum 20000 -Maximum 30000
        if ($script:TakenPorts.ContainsKey($candidate)) { continue }
        $listener = $null
        try {
            $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $candidate)
            $listener.Start()
            $script:TakenPorts[$candidate] = $true
            return $candidate
        } catch { continue } finally { if ($listener) { $listener.Stop() } }
    }
    throw "could not allocate a free port below the ephemeral range"
}

function Wait-ForLogLine([string]$log, [string]$pattern, [int]$seconds, [string]$what, $proc) {
    # Bounded, and it says what it waited for. Reading the node's OWN log rather than
    # the scheduler's counters, because this wait is about one process reaching a
    # state -- the fleet-level wait comes after and asks a different question.
    #
    # It also says WHICH KIND of failure it was, because "waited 300s" alone cannot
    # tell a loaded machine from a wedged process and those are fixed in different
    # places -- one is a budget, the other is a bug. Issue #338's red leg was exactly
    # that question and the log could not answer it: worker A had logged "computing
    # the toolchain fingerprint" and nothing further, which is what a cold include
    # walk on a contended runner looks like AND what a deadlock looks like.
    #
    # Three things separate them, and all three are free:
    #
    #   the log still GROWING at the deadline  -> progress; the budget is too small
    #   the log static while the process lives -> no progress; suspect a hang
    #   the process gone                       -> it died rather than hung, and the
    #                                             exit code is the story
    #
    # And on SUCCESS it prints what the wait actually cost. Nothing recorded that
    # before, so no budget here could be set from data -- the numbers were guesses
    # that survived by being generous. A green run now measures itself.
    $started = Get-Date
    $deadline = $started.AddSeconds($seconds)
    $lastSize = -1
    $lastGrowth = $started
    $everGrew = $false
    # CPU as well as log growth, because the operation this wait most often covers --
    # walking a compiler's include tree -- logs NOTHING while it runs. A slow walk and
    # a wedged process therefore produce identical logs, so log growth alone would have
    # mis-diagnosed #338 with confidence. CPU separates them: work burns it, a block
    # does not. Sampled from the node itself; a spawned `cl` charges its own process,
    # so a low figure here is weaker evidence than a high one, and the verdict below
    # says so rather than pretending otherwise.
    $cpuAtStart = $null
    if ($proc) { try { $proc.Refresh(); $cpuAtStart = $proc.TotalProcessorTime } catch {} }
    while ((Get-Date) -lt $deadline) {
        $text = Get-Content -Raw $log -ErrorAction SilentlyContinue
        if ($text -and $text -match $pattern) {
            Write-Host ("  waited {0}s of {1}s for {2}" -f [int]((Get-Date) - $started).TotalSeconds, $seconds, $what)
            return $true
        }
        $size = if ($text) { $text.Length } else { 0 }
        if ($size -ne $lastSize) {
            if ($lastSize -ge 0) { $everGrew = $true }
            $lastSize = $size; $lastGrowth = Get-Date
        }
        if ($proc -and $proc.HasExited) {
            Write-Host ("FAILED after {0}s waiting for {1}: the process EXITED with code {2}. It did not hang -- it died, and the log below is why." `
                        -f [int]((Get-Date) - $started).TotalSeconds, $what, $proc.ExitCode)
            Write-Host (Get-Content -Raw $log -ErrorAction SilentlyContinue)
            return $false
        }
        Start-Sleep -Milliseconds 400
    }
    $stalledFor = [int]((Get-Date) - $lastGrowth).TotalSeconds
    # Scaled to the budget rather than fixed: a 30s stall means nothing in a 6s wait
    # and everything in a 300s one. Ten percent, floored at five seconds so a short
    # wait still has a usable threshold.
    $stallLimit = [Math]::Max(5, [int]($seconds * 0.1))
    $cpuUsed = $null
    if ($proc -and $cpuAtStart -ne $null) {
        try { $proc.Refresh(); $cpuUsed = ($proc.TotalProcessorTime - $cpuAtStart).TotalSeconds } catch {}
    }
    Write-Host "waited ${seconds}s for $what"
    # The measurements first, always, whatever verdict follows: a verdict that turns
    # out wrong is still useful if the numbers it was drawn from are on the page.
    Write-Host ("  evidence: alive={0} logGrew={1} lastGrowth={2}s ago cpuDuringWait={3}" `
                -f $(if ($proc) { -not $proc.HasExited } else { "unknown" }), $everGrew, $stalledFor,
                   $(if ($cpuUsed -ne $null) { "{0:N1}s" -f $cpuUsed } else { "unknown" }))
    if ($proc -and -not $proc.HasExited) {
        $busy = ($cpuUsed -ne $null -and $cpuUsed -ge 1.0)
        $progressing = ($everGrew -and $stalledFor -lt $stallLimit)
        if ($progressing -or $busy) {
            Write-Host ("VERDICT: the process was still WORKING when the budget ran out ({0}). That is a budget or contention problem, not a hang -- raise the budget or reduce what runs beside it." `
                        -f $(if ($busy) { "it consumed CPU throughout" } else { "its log was still growing" }))
        } elseif ($cpuUsed -ne $null) {
            # Deliberately hedged, and the hedge is for whoever reads this at 2am.
            # Low CPU HERE does not mean nothing is working: this wait covers an
            # operation that spawns a compiler, and a spawned `cl` charges its own
            # process. So the first move is to look for a live child, not to go
            # hunting a deadlock that may not exist.
            Write-Host ("VERDICT: the process is alive, wrote nothing new, and consumed essentially no CPU ITSELF ({0:N1}s)." -f $cpuUsed)
            Write-Host ("  Before concluding it is hung: this step SPAWNS a compiler, and a spawned process charges CPU to itself, not to its parent.")
            Write-Host ("  So check for a live child first -- `Get-Process cl,clang,clang-cl -ErrorAction SilentlyContinue` -- and if one is running and busy, this is a SLOW MACHINE, not a hang.")
            Write-Host ("  Only with no busy child does this read as BLOCKED, and then do not simply raise the timeout.")
        } else {
            Write-Host ("VERDICT: INCONCLUSIVE -- the process is alive and quiet, but its CPU could not be sampled, and this wait covers an operation that logs nothing while it runs. Do not read this as either a hang or a slow machine without more evidence.")
        }
    } elseif ($proc) {
        Write-Host ("VERDICT: the process exited with code {0} before the deadline." -f $proc.ExitCode)
    }
    Write-Host (Get-Content -Raw $log -ErrorAction SilentlyContinue)
    return $false
}

function ConvertTo-QuotedArgs([string[]]$arguments) {
    # -ArgumentList joins an array with spaces into ONE command line, so an element
    # CONTAINING a space arrives at the child as two. `cl.exe` lives under
    # `C:\Program Files\...`, so `--toolchain=<path>` reached the node as
    # `--toolchain=C:\Program` plus a stray positional and it exited 2. The same trap
    # scripts/dist-compile-e2e.ps1 records.
    return $arguments | ForEach-Object {
        if ($_ -match '\s' -and $_ -notmatch '^"') { '"' + $_ + '"' } else { $_ }
    }
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("fc-scratch-iso-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function New-SlowSource([string]$path, [int]$seed) {
    # Heavy standard headers, not a constexpr tower: MSVC folds the latter away in
    # milliseconds, and a compile that fast never overlaps its sibling.
    $text = @"
// distinct per client: $seed
#include <regex>
#include <ranges>
#include <algorithm>
#include <string>
#include <map>
#include <sstream>
long long marker_$seed(std::string const& s) {
    std::regex const re { "([a-z]+)([0-9]*)$seed" };
    std::smatch m;
    long long n = $seed;
    if (std::regex_search(s, m, re)) n += static_cast<long long>(m.size());
    std::map<std::string, long long> counts;
    for (auto const& part: std::views::split(s, ' ')) counts[std::string(part.begin(), part.end())] += $seed;
    for (auto const& [k, v]: counts) n += v + static_cast<long long>(k.size());
    std::ostringstream out; out << n << "-$seed";
    return n + static_cast<long long>(out.str().size());
}
"@
    Set-Content -LiteralPath $path -Value $text -Encoding utf8
}

$clientBody = {
    param($Launcher, $cc, $proj, $obj, $src, $sched, $cache, $tag)
    function ConvertTo-QuotedArgs([string[]]$arguments) {
        return $arguments | ForEach-Object {
            if ($_ -match '\s' -and $_ -notmatch '^"') { '"' + $_ + '"' } else { $_ }
        }
    }
    $env:FASTCACHE_ADDR       = "127.0.0.1:$cache"
    $env:FASTCACHE_SOURCE_DIR = $proj
    $env:FASTCACHE_BINARY_DIR = (Join-Path $proj "build")
    $env:FASTCACHE_VERBOSE    = "1"
    $env:FASTCACHE_SCHEDULER  = $sched
    # BOTH streams to files: `cl` echoes the source name on stdout, and an
    # un-redirected child inside Start-Job puts that into the job's output stream
    # where Receive-Job fails trying to deserialize it.
    $errFile = [IO.Path]::GetTempFileName()
    $outFile = [IO.Path]::GetTempFileName()
    $started = Get-Date
    $p = Start-Process -FilePath $Launcher `
         -ArgumentList (ConvertTo-QuotedArgs @($cc, "/nologo", "/c", "/std:c++20", "/EHsc", "/O2", "/Fo$obj", $src)) `
         -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile -RedirectStandardOutput $outFile
    $e = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $errFile, $outFile -ErrorAction SilentlyContinue
    [pscustomobject]@{ tag = $tag; code = $p.ExitCode; stderr = $e; startedAt = $started; endedAt = (Get-Date) }
}

function Invoke-Phase([string]$label, [bool]$separateTempForB) {
    Write-Host "--- $label"

    $cachePort = Get-FreePort; $schedPort = Get-FreePort; $schedWork = Get-FreePort
    $workerA   = Get-FreePort; $workerB   = Get-FreePort; $adminPort = Get-FreePort

    $phaseDir = Join-Path $scratch $label
    $proj = Join-Path $phaseDir "proj"
    New-Item -ItemType Directory -Force -Path (Join-Path $proj "build") | Out-Null
    $srcA = Join-Path $proj "a.cpp"; $srcB = Join-Path $proj "b.cpp"
    New-SlowSource $srcA 101
    New-SlowSource $srcB 202

    $procs = @()
    try {
        $procs += Start-Process -FilePath $Fastcached -ArgumentList @("--port=$cachePort", "--bind=127.0.0.1") `
                    -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $phaseDir "cache.out.log") `
                    -RedirectStandardError (Join-Path $phaseDir "cache.err.log")

        function Start-NodeIn([string]$name, [string[]]$argv, [string]$tempOverride) {
            $savedTemp = $env:TEMP; $savedTmp = $env:TMP
            if ($tempOverride) {
                New-Item -ItemType Directory -Force -Path $tempOverride | Out-Null
                $env:TEMP = $tempOverride; $env:TMP = $tempOverride
            }
            try {
                return Start-Process -FilePath $Node -ArgumentList (ConvertTo-QuotedArgs $argv) -NoNewWindow -PassThru `
                       -RedirectStandardOutput (Join-Path $phaseDir "$name.out.log") `
                       -RedirectStandardError  (Join-Path $phaseDir "$name.err.log")
            } finally { $env:TEMP = $savedTemp; $env:TMP = $savedTmp }
        }

        # The scheduler's own worker serves a fingerprint no client asks for, so every
        # lease has to land on worker A or worker B.
        $schedProc = Start-NodeIn "sched" @(
            "--listen-scheduler=127.0.0.1:$schedPort", "--fleet-open",
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$schedWork", "--advertise=127.0.0.1:$schedWork",
            "--toolchain=scheduler-only=$Compiler", "--slots=1",
            "--admin-listen=127.0.0.1:$adminPort") $null
        $procs += $schedProc

        # ONE slot each, so the second lease cannot land on the machine already busy
        # -- which is what puts a compile on both processes at the same time.
        # Each worker is started and waited for BEFORE the next one, and that is a
        # deliberate reduction in what this fixture asks of the machine. A bare
        # `--toolchain` makes a node compute a fingerprint by walking the compiler's
        # whole include tree, which is seconds when warm and much longer cold; two of
        # those racing on a two-core CI runner exceeded the budget and the fixture
        # failed having never reached a compile.
        #
        # Serialising costs nothing this test is about. The collision it exists to
        # catch happens when two workers COMPILE at once, which is arranged below --
        # not when they start.
        if (-not (Wait-ForLogLine (Join-Path $phaseDir "sched.err.log") "compile node ready" 180 "the scheduler node to come up" $schedProc)) {
            throw "the scheduler node did not start"
        }

        $workerAProc = Start-NodeIn "workerA" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerA", "--advertise=127.0.0.1:$workerA",
            "--toolchain=$Compiler", "--slots=1") $null
        $procs += $workerAProc
        if (-not (Wait-ForLogLine (Join-Path $phaseDir "workerA.err.log") "compile node ready" 300 "worker A to compute its toolchain fingerprint and bind" $workerAProc)) {
            throw "worker A did not start"
        }

        $bTemp = if ($separateTempForB) { Join-Path $phaseDir "tempB" } else { $null }
        $workerBProc = Start-NodeIn "workerB" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerB", "--advertise=127.0.0.1:$workerB",
            "--toolchain=$Compiler", "--slots=1") $bTemp
        $procs += $workerBProc
        if (-not (Wait-ForLogLine (Join-Path $phaseDir "workerB.err.log") "compile node ready" 300 "worker B to compute its toolchain fingerprint and bind" $workerBProc)) {
            throw "worker B did not start"
        }

        # Asked of the SCHEDULER, bounded, and it says what it waited for. A worker
        # logging "compile node ready" says its own port is bound, not that the
        # scheduler has heard from it -- dispatching on that races the first
        # heartbeat and is refused NoWorker.
        $deadline = (Get-Date).AddSeconds(120); $regs = 0
        while ((Get-Date) -lt $deadline -and $regs -lt 3) {
            Start-Sleep -Milliseconds 700
            try {
                $text = (Invoke-WebRequest -Uri "http://127.0.0.1:$adminPort/metrics" -TimeoutSec 5).Content
                foreach ($line in ($text -split "`n")) {
                    if ($line -match '^fastcached_dispatch_worker_registrations_total\s+([0-9]+)') { $regs = [int]$Matches[1] }
                }
            } catch { }
        }
        if ($regs -lt 3) {
            Write-Host "waited 120s for three worker registrations at the scheduler; saw $regs (all three nodes were already up, so this is heartbeats, not startup)"
            foreach ($n in @("sched", "workerA", "workerB")) {
                Write-Host "--- $n"; Get-Content (Join-Path $phaseDir "$n.err.log") -Tail 20 -ErrorAction SilentlyContinue
            }
            throw "the workers did not register"
        }

        $objA = Join-Path $proj "build\a.obj"; $objB = Join-Path $proj "build\b.obj"
        $jobs = @(
            Start-Job -ScriptBlock $clientBody -ArgumentList $Launcher,$Compiler,$proj,$objA,$srcA,"127.0.0.1:$schedPort",$cachePort,"A"
            Start-Job -ScriptBlock $clientBody -ArgumentList $Launcher,$Compiler,$proj,$objB,$srcB,"127.0.0.1:$schedPort",$cachePort,"B"
        )
        $results = $jobs | Wait-Job -Timeout 300 | Receive-Job
        $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
        if ($results.Count -ne 2) { throw "waited 300s for two dispatched compiles; got $($results.Count)" }

        $problems = @()
        $dispatched = 0
        foreach ($r in ($results | Sort-Object tag)) {
            if ($r.stderr -match "DISPATCHED to ") { $dispatched++ }
            if ($r.stderr -match "reported exit ([0-9]+)") {
                $problems += "client $($r.tag): the worker failed the compile (exit $($Matches[1])) and the client fell back locally"
            }
            if ($r.code -ne 0) { $problems += "client $($r.tag): the launcher exited $($r.code)" }
        }
        if ($dispatched -ne 2) { $problems += "only $dispatched of 2 compiles were dispatched" }

        $a, $b = ($results | Sort-Object tag)
        if (-not (($a.startedAt -lt $b.endedAt) -and ($b.startedAt -lt $a.endedAt))) {
            $problems += "the two compiles did not overlap, so nothing was contended and this phase proves nothing"
        }

        # A swapped object carries the other unit's marker symbol. Checked even when
        # everything above passed: the quiet failure mode is exit 0 and wrong bytes.
        foreach ($pair in @(@{o=$objA;other="marker_202";t="A"}, @{o=$objB;other="marker_101";t="B"})) {
            if (-not (Test-Path $pair.o)) { $problems += "client $($pair.t): no object was written"; continue }
            $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($pair.o))
            if ($text.Contains($pair.other)) { $problems += "client $($pair.t) was handed the OTHER unit's object" }
        }

        if ($problems.Count -gt 0) {
            foreach ($p in $problems) { Write-Host "  FAIL: $p" }
            foreach ($r in $results) { Write-Host "--- client $($r.tag) stderr"; Write-Host $r.stderr }
            foreach ($n in @("workerA", "workerB")) {
                Write-Host "--- $n"; Get-Content (Join-Path $phaseDir "$n.err.log") -Tail 30 -ErrorAction SilentlyContinue
            }
            throw "$label failed"
        }
        Write-Host "  ok: 2 of 2 dispatched, both overlapped, each object its own"
    }
    finally {
        foreach ($p in $procs) { try { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } } catch {} }
        Start-Sleep -Milliseconds 400
    }
}

try {
    # Phase 1 is the defect's own configuration and must now be clean; phase 2 is the
    # control, and a regression that broke both would otherwise look like the
    # environment rather than like this change.
    Invoke-Phase "shared-temp" $false
    Invoke-Phase "separate-temp" $true
    Write-Host "node-scratch-isolation-e2e: PASS"
    exit 0
}
finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
