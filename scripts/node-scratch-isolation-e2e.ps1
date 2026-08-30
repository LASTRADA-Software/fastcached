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
        $procs += Start-NodeIn "sched" @(
            "--listen-scheduler=127.0.0.1:$schedPort", "--fleet-open",
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$schedWork", "--advertise=127.0.0.1:$schedWork",
            "--toolchain=scheduler-only=$Compiler", "--slots=1",
            "--admin-listen=127.0.0.1:$adminPort") $null

        # ONE slot each, so the second lease cannot land on the machine already busy
        # -- which is what puts a compile on both processes at the same time.
        $procs += Start-NodeIn "workerA" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerA", "--advertise=127.0.0.1:$workerA",
            "--toolchain=$Compiler", "--slots=1") $null

        $bTemp = if ($separateTempForB) { Join-Path $phaseDir "tempB" } else { $null }
        $procs += Start-NodeIn "workerB" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerB", "--advertise=127.0.0.1:$workerB",
            "--toolchain=$Compiler", "--slots=1") $bTemp

        # Asked of the SCHEDULER, bounded, and it says what it waited for. A worker
        # logging "compile node ready" says its own port is bound, not that the
        # scheduler has heard from it -- dispatching on that races the first
        # heartbeat and is refused NoWorker.
        $deadline = (Get-Date).AddSeconds(90); $regs = 0
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
            Write-Host "waited 90s for three worker registrations at the scheduler; saw $regs"
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
