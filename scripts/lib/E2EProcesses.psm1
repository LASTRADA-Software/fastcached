# SPDX-License-Identifier: Apache-2.0
#
# Tearing down what a PowerShell e2e fixture spawned: kill it, confirm it EXITED,
# and name what did not.
#
# ## Why this is a module rather than two copies
#
# `dist-compile-e2e.ps1` ignored what `WaitForExit` answered, so an isolation
# worker that outlived the bound was still holding `iso-worker.log` when the next
# driver's `Remove-Item` reached it -- a teardown overlap reported as a fault of
# the case that ran next, with nothing naming the process.
# `node-scratch-isolation-e2e.ps1` had the same teardown, confirming nothing. A
# fix private to one fixture fixes one fixture: `.agent/rules/testing.md` says why
# a first failure MASKS its identical siblings, and `E2EPorts.psm1` is where the
# port half of the same lesson already lives.
#
# ## What a teardown owes, and what it does not
#
# A BOUND, not a guarantee: a killed process on a loaded Windows host has been seen
# to outlive five seconds, and raising the bound only moves the line. What a
# survivor may cost is a line saying so -- so the fixture must also not SHARE a
# directory or a port with whatever runs next (per run, per driver, per phase).
# The per-run half is here too: `New-E2ERunRoot` claims a root no run reuses, and
# `Clear-E2EStaleRoots` removes old ones and NAMES what holds any that will not go.

Set-StrictMode -Version Latest

# Kill each process, then confirm each one EXITED within ONE shared bound.
#
# Returns a line for every process that did not, naming its PID and what it was
# started as (`CommandLines`, keyed by PID), and nothing when every one is gone.
# There are three answers per process, not two: exited, still running, and "could
# not be asked" -- WaitForExit throws on a handle that is gone and HasExited then
# reads `$null`, and reading either as "exited" is how a survivor goes unreported.
#
# Nothing here throws: teardown runs on every exit path INCLUDING the failing
# ones, and anything thrown would replace the real diagnostic with a secondary one
# about tearing down.
#
# @param Processes         What to stop; `$null` entries are skipped.
# @param CommandLines      PID -> the command line it was started with.
# @param BoundMilliseconds The total wait, shared by every process, after the kills.
# @param Kill              Called once per process that has not exited. A parameter
#                          so the DECISION can be driven without a process to kill.
# @return One line per process not confirmed gone.
function Stop-E2EProcesses {
    param(
        [object[]]$Processes,
        [hashtable]$CommandLines = @{},
        [int]$BoundMilliseconds = 5000,
        [scriptblock]$Kill = { param($p) $p.Kill() }
    )
    $live = @($Processes | Where-Object { $null -ne $_ })
    foreach ($p in $live) {
        # A process that exited between the check and the kill throws.
        try { if (-not $p.HasExited) { & $Kill $p } } catch { $null = $_ }
    }
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $report = @()
    foreach ($p in $live) {
        $remaining = [Math]::Max(0, $BoundMilliseconds - [int]$clock.ElapsedMilliseconds)
        $state = "unknown"
        $why = ""
        try { $state = if ($p.WaitForExit($remaining)) { "exited" } else { "running" } }
        catch {
            $why = $_.Exception.Message
            # Only a BOOLEAN is an answer. PowerShell turns an exception in a property
            # getter into `$null` rather than throwing -- measured on a Process with no
            # handle -- so a truthiness test would read "could not be asked" as "running".
            $answer = $p.HasExited
            if ($answer -is [bool]) { $state = if ($answer) { "exited" } else { "running" } }
        }
        if ($state -eq "exited") { continue }
        # The PID is read the same way and can be `$null` too, on a Process that never
        # had a handle -- and a `$null` key makes ContainsKey THROW.
        $id = $p.Id
        $who = if ($null -ne $id) { "PID $id" } else { "a process whose PID cannot be read" }
        $started = if ($null -ne $id -and $CommandLines.ContainsKey($id)) { $CommandLines[$id] } else { "(no recorded command line)" }
        if ($state -eq "running") {
            $report += "$who was still running $BoundMilliseconds ms after its kill: $started"
        } else {
            $report += "$who could not be asked whether it exited ($why): $started"
        }
    }
    return $report
}

# ---------------------------------------------------------------------------
# Scratch roots: one per RUN, claimed, never reused
# ---------------------------------------------------------------------------

# The file a run holds open, with no sharing, for as long as it owns its root.
$script:ClaimFile = ".claim"

# Claim a fresh scratch root for this run under `Base`, never one an earlier run used.
#
# Per RUN, because a survivor outlives its run's teardown bound as readily as its
# driver's: with one root shared between runs, run N returned with twelve survivors
# and an immediate run N+1 died in 0 s on "raft-log ... being used by another
# process" -- naming a file, with the holders' PIDs only in run N's log.
#
# Unique by an EXCLUSIVE create of the claim file (`CreateNew`), not by a random name
# and not by `New-Item`, which checks and then creates and so lets two runs both
# "succeed". The claim is then HELD, with no sharing, so a sweep can tell a live
# run's root from a dead one's.
#
# @param Base The fixture's base directory; created when absent.
# @return @{ Path = <the root>; Claim = <the open claim; Dispose() releases it> }
function New-E2ERunRoot {
    param([Parameter(Mandatory)][string]$Base)
    New-Item -ItemType Directory -Force -Path $Base | Out-Null
    $stem = "run-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $PID
    foreach ($attempt in 0..99) {
        $path = Join-Path $Base ($(if ($attempt -eq 0) { $stem } else { "$stem-$attempt" }))
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        try {
            $claim = [System.IO.File]::Open((Join-Path $path $script:ClaimFile), [System.IO.FileMode]::CreateNew,
                                            [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            return @{ Path = $path; Claim = $claim }
        } catch [System.IO.IOException] {
            # Somebody else's claim: the next name.
            $null = $_
        }
    }
    throw "could not claim a scratch root under $Base in 100 attempts"
}

# Whether a live run still holds `Root`'s claim.
#
# A root with no claim file is not a live run's -- an older layout, or a run that
# died before claiming -- and one whose claim opens exclusively is a dead run's.
# Anything that stops the open, a live holder or not, answers HELD: leaving a root
# alone is the direction that cannot break a run.
#
# @param Root One entry under the base.
# @return $true when the root must be left alone.
function Test-E2ERootClaimed([string]$Root) {
    $claim = Join-Path $Root $script:ClaimFile
    if (-not (Test-Path -LiteralPath $claim -PathType Leaf)) { return $false }
    try {
        [System.IO.File]::Open($claim, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite,
                               [System.IO.FileShare]::None).Dispose()
        return $false
    } catch {
        return $true
    }
}

# Every process on this host with its command line, for naming what holds a root.
#
# @return @{ Listed = $true; Processes = @(@{ Id; CommandLine }...) }, or Listed = $false
#         when this platform cannot say -- which is not the same answer as "none".
function Get-E2EProcessCommandLines {
    if ($IsWindows) {
        try {
            $all = @(Get-CimInstance Win32_Process -ErrorAction Stop | ForEach-Object {
                [pscustomobject]@{ Id = [int]$_.ProcessId; CommandLine = [string]$_.CommandLine } })
            return @{ Listed = $true; Processes = $all }
        } catch {
            return @{ Listed = $false; Processes = @() }
        }
    }
    if (Test-Path -LiteralPath "/proc" -PathType Container) {
        $all = @(Get-ChildItem -LiteralPath "/proc" -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^[0-9]+$' } | ForEach-Object {
                $text = $null
                try { $text = [System.IO.File]::ReadAllText("/proc/$($_.Name)/cmdline") } catch { $null = $_ }
                if ($null -ne $text) { [pscustomobject]@{ Id = [int]$_.Name; CommandLine = $text.Replace([char]0, ' ').Trim() } }
            })
        return @{ Listed = $true; Processes = $all }
    }
    return @{ Listed = $false; Processes = @() }
}

# Remove the roots earlier runs left under `Base`, and REPORT each one that would not go.
#
# Best effort and never fatal: now that no run reuses a root, one that will not go
# costs disk, not correctness. What it must not cost is the NAME of its holder, so
# each failure names the processes whose command line mentions the root -- the one
# link from a held file back to a process that needs no handle tool. A root whose
# claim is still held is a live run's and is left alone, and says so.
#
# @param Base          The fixture's base directory.
# @param ListProcesses Returns what `Get-E2EProcessCommandLines` does. A parameter,
#                      like `Remove`, so the decision is driven without a real holder.
# @param Remove        Removes one root, throwing when it cannot.
# @return One line per root not removed.
function Clear-E2EStaleRoots {
    param(
        [Parameter(Mandatory)][string]$Base,
        [scriptblock]$ListProcesses = { Get-E2EProcessCommandLines },
        [scriptblock]$Remove = { param($path) Remove-Item -LiteralPath $path -Recurse -Force -ErrorAction Stop }
    )
    $report = @()
    if (-not (Test-Path -LiteralPath $Base -PathType Container)) { return $report }
    $listing = $null
    foreach ($entry in @(Get-ChildItem -LiteralPath $Base -Force)) {
        $old = $entry.FullName
        if (Test-E2ERootClaimed $old) {
            $report += "left $old alone: a live run holds its claim"
            continue
        }
        $why = ""
        try { & $Remove $old; continue } catch { $why = $_.Exception.Message }
        if ($null -eq $listing) { $listing = & $ListProcesses }
        if (-not $listing.Listed) {
            $report += "could not remove $old ($why); the processes on this host could not be listed to find its holder"
            continue
        }
        $holders = @($listing.Processes | Where-Object {
            $_.CommandLine -and $_.CommandLine.IndexOf($old, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 })
        if ($holders.Count -eq 0) {
            $report += "could not remove $old ($why); no running process names it on its command line"
        } else {
            $named = ($holders | ForEach-Object { "PID $($_.Id): $($_.CommandLine)" }) -join "; "
            $report += "could not remove $old ($why); held by: $named"
        }
    }
    return $report
}

# The decision, every arm, with stand-ins rather than processes.
#
# Each stand-in answers WaitForExit and HasExited the way a process in that state
# does, and records what it was asked. Run by every fixture that imports this
# module, from its own `-SelfTest`, so each one also proves its import works.
#
# @return @{ Cases = <run>; Failures = <failed> }, after printing one line per case.
function Invoke-E2EProcessesSelfTest {
    $script:ProcessCases = 0
    $script:ProcessFailures = 0
    function Check([bool]$condition, [string]$what) {
        $script:ProcessCases++
        if ($condition) { Write-Host "   ok   $what" }
        else { Write-Host "   FAIL $what"; $script:ProcessFailures++ }
    }
    function New-StandIn([int]$id, [scriptblock]$waitForExit, [scriptblock]$hasExited) {
        $s = [pscustomobject]@{ Id = $id; Waits = [System.Collections.Generic.List[int]]::new() }
        $s | Add-Member -MemberType ScriptMethod -Name WaitForExit -Value $waitForExit
        $s | Add-Member -MemberType ScriptProperty -Name HasExited -Value $hasExited
        return $s
    }
    $killed = [System.Collections.Generic.List[int]]::new()
    $recordKill = { param($p) $killed.Add($p.Id) }.GetNewClosure()
    $started = @{ 4242 = "fastcache-compile-node.exe --listen-node=127.0.0.1:1"; 4343 = "fastcached.exe --listen=127.0.0.1:2" }

    # The POSITIVE direction first: a process that goes is killed and not named.
    $gone = New-StandIn 4242 { param($ms) $this.Waits.Add($ms); $true } { $false }
    $r = @(Stop-E2EProcesses @($gone, $null) $started 5000 $recordKill)
    Check ($r.Count -eq 0 -and $killed.Count -eq 1) "a process that exits within the bound is killed once and reported by nobody"

    $killed.Clear()
    $already = New-StandIn 4242 { param($ms) $true } { $true }
    $r = @(Stop-E2EProcesses @($already) $started 5000 $recordKill)
    Check ($r.Count -eq 0 -and $killed.Count -eq 0) "one that has already exited is neither killed nor reported"

    # THE CASE THIS IS ABOUT: the bound runs out with the process still there.
    $stays = New-StandIn 4242 { param($ms) $false } { $false }
    $r = @(Stop-E2EProcesses @($stays) $started 5000 $recordKill)
    Check ($r.Count -eq 1) "a process still running when the bound runs out is REPORTED"
    Check ($r.Count -eq 1 -and $r[0] -like "PID 4242 was still running 5000 ms after its kill: fastcache-compile-node.exe --listen-node=127.0.0.1:1") "by PID and by the command line it was started with"

    # A third answer, not folded into either of the others. A getter that throws
    # reads `$null` here, exactly as a real Process's HasExited does with no handle.
    $mute = New-StandIn 4343 { param($ms) throw "no process is associated" } { throw "no process is associated" }
    $r = @(Stop-E2EProcesses @($mute) $started 5000 $recordKill)
    Check ($r.Count -eq 1 -and $r[0] -like "PID 4343 could not be asked whether it exited (*no process is associated*): fastcached.exe --listen=127.0.0.1:2") "one that cannot be asked is reported as THAT, not as gone"

    # And a REAL Process with no handle, not a stand-in: its Id, HasExited and
    # WaitForExit all fail, which is what the two arms above were written against.
    # A throw is caught here and FAILS the case, rather than ending the self-test
    # with every later case unrun.
    try { $r = @(Stop-E2EProcesses @([System.Diagnostics.Process]::new()) $started 5000 $recordKill) }
    catch { $r = @("threw: $($_.Exception.Message)") }
    Check ($r.Count -eq 1 -and $r[0] -like "a process whose PID cannot be read could not be asked whether it exited (*): (no recorded command line)") "a Process with no handle is reported, and reporting it does not throw"

    $unrecorded = New-StandIn 4444 { param($ms) $false } { $false }
    $r = @(Stop-E2EProcesses @($unrecorded) $started 5000 $recordKill)
    Check ($r.Count -eq 1 -and $r[0] -like "PID 4444 * (no recorded command line)") "a PID nothing recorded says so rather than printing nothing"

    # ONE bound for the whole teardown: a stand-in that spends all of its wait,
    # and then some, leaves nothing of the bound to the next.
    $slow1 = New-StandIn 4242 { param($ms) $this.Waits.Add($ms); Start-Sleep -Milliseconds ($ms + 50); $false } { $false }
    $slow2 = New-StandIn 4343 { param($ms) $this.Waits.Add($ms); $false } { $false }
    $r = @(Stop-E2EProcesses @($slow1, $slow2) $started 300 $recordKill)
    Check ($r.Count -eq 2 -and $slow1.Waits[0] -le 300 -and $slow2.Waits[0] -eq 0) "the bound is shared by every process, not granted to each"

    # ---- scratch roots ------------------------------------------------------
    #
    # Over a REAL directory and REAL claims; only the holder and its refusal are
    # stand-ins, since a live holder is exactly what a self-test must not need.
    $base = Join-Path ([System.IO.Path]::GetTempPath()) ("e2e-roots-selftest-" + [System.Guid]::NewGuid().ToString("N"))
    $noProcesses = { @{ Listed = $true; Processes = @() } }
    try {
        $a = New-E2ERunRoot -Base $base
        $b = New-E2ERunRoot -Base $base
        Check ($a.Path -ne $b.Path -and (Test-Path -LiteralPath $a.Path) -and (Test-Path -LiteralPath $b.Path)) "two runs in the same second claim two different roots"

        $r = @(Clear-E2EStaleRoots -Base $base -ListProcesses $noProcesses)
        Check ($r.Count -eq 2 -and @($r | Where-Object { $_ -like "left * alone: a live run holds its claim" }).Count -eq 2 -and (Test-Path -LiteralPath $a.Path)) "a root whose claim is HELD is left alone, and the sweep says so"

        # The POSITIVE direction: released, the same roots go, and nothing is said.
        $a.Claim.Dispose(); $b.Claim.Dispose()
        $r = @(Clear-E2EStaleRoots -Base $base -ListProcesses $noProcesses)
        Check ($r.Count -eq 0 -and -not (Test-Path -LiteralPath $a.Path) -and -not (Test-Path -LiteralPath $b.Path)) "a released root is removed, and reported by nobody"

        # THE CASE THIS IS ABOUT: an old root something still holds.
        $held = Join-Path $base "run-old"
        New-Item -ItemType Directory -Path $held | Out-Null
        $holder = { @{ Listed = $true; Processes = @(
            [pscustomobject]@{ Id = 4242; CommandLine = "fastcache-compile-node.exe --cluster-dir=$held\iso-scheduler.state" },
            [pscustomobject]@{ Id = 7; CommandLine = "unrelated.exe --elsewhere" }) } }.GetNewClosure()
        $refuse = { param($path) throw "The process cannot access the file '$path\iso-scheduler.state\raft-log' because it is being used by another process." }
        $r = @(Clear-E2EStaleRoots -Base $base -ListProcesses $holder -Remove $refuse)
        Check ($r.Count -eq 1 -and $r[0] -like "could not remove $held (*being used by another process*); held by: PID 4242: fastcache-compile-node.exe --cluster-dir=*") "an old root something holds is REPORTED, naming the holder by PID and command line"
        Check ($r.Count -eq 1 -and $r[0] -notlike "*PID 7*") "and naming only the processes whose command line mentions that root"

        $r = @(Clear-E2EStaleRoots -Base $base -ListProcesses $noProcesses -Remove $refuse)
        Check ($r.Count -eq 1 -and $r[0] -like "could not remove $held (*); no running process names it on its command line") "a held root nobody names says THAT, rather than naming nobody"
        $cannot = { @{ Listed = $false; Processes = @() } }
        $r = @(Clear-E2EStaleRoots -Base $base -ListProcesses $cannot -Remove $refuse)
        Check ($r.Count -eq 1 -and $r[0] -like "could not remove $held (*); the processes on this host could not be listed*") "and a host that cannot list its processes is not read as one with no holder"
    } finally {
        Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
    }

    # The real lister, asked for something it must find: this very process.
    $listing = Get-E2EProcessCommandLines
    Check ($listing.Listed -and @($listing.Processes | Where-Object { $_.Id -eq $PID -and $_.CommandLine }).Count -eq 1) "the default lister finds this very process, with a command line"

    return @{ Cases = $script:ProcessCases; Failures = $script:ProcessFailures }
}

Export-ModuleMember -Function Stop-E2EProcesses, New-E2ERunRoot, Clear-E2EStaleRoots, Get-E2EProcessCommandLines,
                              Invoke-E2EProcessesSelfTest
