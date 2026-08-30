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
    [string]$Compiler,
    # The wait classifier only, against synthetic processes. Drives no node, opens
    # no socket and runs no compiler, so it belongs in the default ctest set -- the
    # same reasoning `dist-compile-e2e.ps1 -SelfTest` records for itself. It exists
    # because a fixture's own logic is the one thing nothing else tests, and this
    # particular logic reported WORKING for a process that had done nothing for five
    # minutes (#354).
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$SkipExit = 77

function Skip([string]$why) {
    Write-Host "node-scratch-isolation-e2e: $why -- skipping"
    exit $SkipExit
}

# Skipped entirely under -SelfTest, which drives none of these: requiring a built
# node to exercise a wait's arithmetic would put the one check that has no
# prerequisites behind every prerequisite there is.
if (-not $SelfTest) {
    foreach ($pair in @(@{p=$Fastcached;n="-Fastcached"}, @{p=$Node;n="-Node"}, @{p=$Launcher;n="-Launcher"})) {
        if (-not $pair.p -or -not (Test-Path $pair.p)) { Skip "$($pair.n) was not given a built binary" }
    }
    if (-not $Compiler) { $Compiler = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source }
    if (-not $Compiler -or -not (Test-Path $Compiler)) { Skip "no MSVC cl.exe on PATH" }
}

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

function Get-ProcessTreeCpu([int]$rootPid, $rootStarted) {
    # The CPU of every DESCENDANT of a process, and how many there are.
    #
    # One CIM query answers parentage and CPU for every process on the box at once,
    # so a whole subtree costs a single call rather than one `Get-Process` per child.
    # It is sampled on its own, slower cadence than the log: the query enumerates
    # around 350 processes and costs roughly 190ms on a developer machine, which is
    # fine every few seconds and is not fine every 400ms. An instrument that loads
    # the machine it is measuring changes the answer it is measuring.
    #
    # Returns $null when the query fails, which is NOT the same as a tree with no
    # CPU in it -- the caller must not read "could not see" as "saw nothing", which
    # is the whole defect this file exists to stop repeating.
    try {
        $all = Get-CimInstance Win32_Process `
                   -Property ProcessId, ParentProcessId, KernelModeTime, UserModeTime, CreationDate `
                   -ErrorAction Stop
    } catch { return $null }

    $byParent = @{}
    foreach ($entry in $all) {
        $key = [int]$entry.ParentProcessId
        if (-not $byParent.ContainsKey($key)) { $byParent[$key] = [System.Collections.Generic.List[object]]::new() }
        $byParent[$key].Add($entry)
    }

    $cpu = 0.0
    $count = 0
    $pids = [System.Collections.Generic.List[int]]::new()
    $seen = @{ $rootPid = $true }
    $frontier = @($rootPid)
    while ($frontier.Count -gt 0) {
        $next = [System.Collections.Generic.List[object]]::new()
        foreach ($parent in $frontier) {
            if (-not $byParent.ContainsKey([int]$parent)) { continue }
            foreach ($child in $byParent[[int]$parent]) {
                $childPid = [int]$child.ProcessId
                if ($seen.ContainsKey($childPid)) { continue }
                # Windows recycles process ids, so over a five-minute wait an
                # unrelated process can inherit a dead child's number and appear to
                # name our process as its parent. A process that STARTED BEFORE the
                # root is not the root's descendant, whatever the id says.
                if ($rootStarted -and $child.CreationDate -and $child.CreationDate -lt $rootStarted) { continue }
                $seen[$childPid] = $true
                $count++
                $pids.Add($childPid)
                $cpu += ([double]$child.KernelModeTime + [double]$child.UserModeTime) / 1e7
                $next.Add($childPid)
            }
        }
        $frontier = $next
    }
    # The ids as well as the totals, so a caller that has to CLEAN UP a tree reaches
    # it through the same enumeration the measurement uses rather than a second one.
    return [pscustomobject]@{ Cpu = $cpu; Count = $count; Pids = $pids }
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
    # And on SUCCESS it prints what the wait actually cost. Nothing recorded that
    # before, so no budget here could be set from data -- the numbers were guesses
    # that survived by being generous. A green run now measures itself.
    #
    # ------------------------------------------------------------------------
    # What the first version of this classifier got wrong, and why the shape here
    # is the fix rather than a tuning of it (#354's first real sighting):
    #
    #     evidence: alive=True logGrew=True lastGrowth=300s ago cpuDuringWait=3.4s
    #     VERDICT: the process was still WORKING (it consumed CPU throughout)
    #
    # Three separate errors, and the third is the one worth remembering.
    #
    # 1. `cpuDuringWait` was CUMULATIVE over the whole wait and was read as a claim
    #    about the process's state AT THE DEADLINE. 3.4s spread evenly over 300s and
    #    3.4s burned in the first ten seconds followed by a wedge are the same
    #    number and opposite diagnoses. A duty cycle over that same window does not
    #    fix it -- it is the same number divided by the same 300. Only a RECENT
    #    window can answer a question about now, so that is what the verdict below
    #    is drawn from and the totals are printed as evidence only.
    #
    # 2. No MAGNITUDE bar can be calibrated here. The whole Windows SDK include tree
    #    is 9,487 files and walks warm in 0.1s at 88% duty; the same walk on a cold,
    #    virus-scanned, contended runner is I/O bound and burns a tiny fraction of
    #    that. Healthy duty for this one operation spans two orders of magnitude, so
    #    a bar set high enough to exclude idle noise would call a genuinely working
    #    cold walk BLOCKED -- and send somebody hunting a hang that is not there.
    #    What does not vary is ZERO: a blocked thread accrues no CPU at any
    #    temperature. So the test is PRESENCE in a recent window, not magnitude, and
    #    everything between "idle" and "clearly working" is reported as neither.
    #
    # 3. The two signals CONTRADICTED each other and an `-or` let the weaker one win
    #    unopposed: `$progressing` was False (the log had not grown in 300s) and
    #    `$busy` was True (3.4s >= 1.0s), and the disjunction reported WORKING. An
    #    `-or` is right for two independent CONFIRMATIONS and wrong for two
    #    competing READINGS. `logGrew=True` was true and uninformative -- it counted
    #    the two startup lines landing in different polls -- and printing it beside a
    #    conclusion lent it authority it had not earned. A signal that cannot be
    #    false in the failing case is not evidence.
    #
    # The process TREE is not a refinement of this, it is required for correctness:
    # between the fingerprint line and "compile node ready" the node spawns `cl` two
    # or three times and then sits in a kernel wait at zero own-CPU while the child
    # works. Recent-window own-CPU alone would call that BLOCKED, which is error 2
    # again from the other end. The earlier version documented this limitation in
    # prose and told the reader to run `Get-Process cl` by hand; the instrument does
    # it now.
    #
    # No verdict here prescribes a remedy. "Raise the budget" is sound advice in
    # general, was printed by the branch that fires most often, and is precisely
    # what #354 refuses -- that budget has been raised twice already. An instrument
    # that prints a remedy has to know when the remedy is under dispute, and this
    # one cannot, so it states the finding and stops.
    $started = Get-Date
    $deadline = $started.AddSeconds($seconds)
    $lastSize = -1
    $lastGrowth = $started
    $everGrew = $false

    # Scaled to the budget rather than fixed: a 30s stall means nothing in a 6s wait
    # and everything in a 300s one. Ten percent, floored at five seconds so a short
    # wait still has a usable threshold.
    $stallLimit = [Math]::Max(5, [int]($seconds * 0.1))

    # The window the CPU verdict is drawn from, and the two bounds it is judged
    # against. Both bounds are printed beside the verdict, because they are
    # judgements rather than measurements and the next reader should be able to
    # disagree with them from the numbers on the page.
    #
    #   floor -- at or below this, indistinguishable from an idle .NET runtime's own
    #            timer and GC threads (measured: 0.062s over 12s for an idle pwsh).
    #   clear -- at or above this, activity no idle process produces.
    #
    # Between them this instrument genuinely cannot tell, and says so.
    $window = [Math]::Min($seconds, [Math]::Max(30, [int]($seconds / 4)))
    $idleFloor = [Math]::Max(0.15, $window * 0.002)
    $clearlyBusy = [Math]::Max(0.50, $window * 0.010)

    $rootPid = $null
    $rootStarted = $null
    if ($proc) { try { $rootPid = $proc.Id; $rootStarted = $proc.StartTime } catch {} }

    $ownSamples = [System.Collections.Generic.List[object]]::new()
    $treeSamples = [System.Collections.Generic.List[object]]::new()
    # Often enough that a short wait still gets several samples, rarely enough that
    # the query does not become the load.
    $treeEvery = [TimeSpan]::FromSeconds([Math]::Max(1, [Math]::Min(5, [int]($seconds / 6))))
    $treeDue = $started

    while ($true) {
        $now = Get-Date
        if ($proc) {
            try {
                $proc.Refresh()
                $ownSamples.Add([pscustomobject]@{ At = $now; Cpu = $proc.TotalProcessorTime.TotalSeconds })
            } catch {}
        }
        if ($rootPid -and $now -ge $treeDue) {
            $treeDue = $now.Add($treeEvery)
            $tree = Get-ProcessTreeCpu $rootPid $rootStarted
            if ($tree) { $treeSamples.Add([pscustomobject]@{ At = $now; Cpu = $tree.Cpu; Count = $tree.Count }) }
        }

        $text = Get-Content -Raw $log -ErrorAction SilentlyContinue
        if ($text -and $text -match $pattern) {
            Write-Host ("  waited {0}s of {1}s for {2}" -f [int]((Get-Date) - $started).TotalSeconds, $seconds, $what)
            return $true
        }
        # Only a read that SUCCEEDED may move the growth clock. A log briefly locked
        # by the writer reads as $null, and treating that as a size change would
        # have a wedged process report progress every time its log could not be
        # opened -- false progress, in the one direction that matters.
        if ($null -ne $text) {
            $size = $text.Length
            if ($size -ne $lastSize) {
                if ($lastSize -ge 0) { $everGrew = $true }
                $lastSize = $size
                $lastGrowth = Get-Date
            }
        }

        if ($proc -and $proc.HasExited) {
            Write-Host ("FAILED after {0}s waiting for {1}: the process EXITED with code {2}. It did not hang -- it died, and the log below is why." `
                        -f [int]((Get-Date) - $started).TotalSeconds, $what, $proc.ExitCode)
            Write-Host (Get-Content -Raw $log -ErrorAction SilentlyContinue)
            return $false
        }
        if ((Get-Date) -ge $deadline) { break }
        Start-Sleep -Milliseconds 400
    }

    $stalledFor = [int]((Get-Date) - $lastGrowth).TotalSeconds
    $cutoff = (Get-Date).AddSeconds(-$window)

    # Own CPU is monotonic, so the recent figure is a plain difference against the
    # first sample inside the window.
    $ownTotal = $null
    $ownRecent = $null
    if ($ownSamples.Count -ge 2) {
        $last = $ownSamples[$ownSamples.Count - 1]
        $ownTotal = $last.Cpu - $ownSamples[0].Cpu
        $base = $last
        foreach ($sample in $ownSamples) { if ($sample.At -ge $cutoff) { $base = $sample; break } }
        $ownRecent = $last.Cpu - $base.Cpu
    }

    # A tree's total is NOT monotonic: a child that exits takes its CPU out of the
    # sum. So the recent figure accumulates the positive steps rather than
    # differencing the endpoints -- a child ending is activity too, but it is not
    # activity this can measure, and it must never be subtracted from a sibling's
    # real work.
    $childRecent = $null
    $childrenNow = $null
    $childrenSeen = 0
    if ($treeSamples.Count -ge 2) {
        $childRecent = 0.0
        foreach ($index in 1..($treeSamples.Count - 1)) {
            if ($treeSamples[$index].At -lt $cutoff) { continue }
            $step = $treeSamples[$index].Cpu - $treeSamples[$index - 1].Cpu
            if ($step -gt 0) { $childRecent += $step }
        }
        $childrenNow = $treeSamples[$treeSamples.Count - 1].Count
        foreach ($sample in $treeSamples) {
            if ($sample.At -ge $cutoff -and $sample.Count -gt $childrenSeen) { $childrenSeen = $sample.Count }
        }
    }

    $show = { param($value) if ($null -ne $value) { "{0:N2}s" -f $value } else { "unknown" } }

    Write-Host "waited ${seconds}s for $what"
    # The measurements first, always, whatever verdict follows: a verdict that turns
    # out wrong is still useful if the numbers it was drawn from are on the page.
    Write-Host ("  evidence: alive={0} log grew at all={1}, last growth {2}s ago (a stall over {3}s is no longer progress)" `
                -f $(if ($proc) { -not $proc.HasExited } else { "unknown" }), $everGrew, $stalledFor, $stallLimit)
    Write-Host ("  evidence: own cpu {0} over the whole wait, {1} in the last {2}s" `
                -f (& $show $ownTotal), (& $show $ownRecent), $window)
    Write-Host ("  evidence: descendants now={0}, most seen in the window={1}, their cpu in the last {2}s={3}" `
                -f $(if ($null -ne $childrenNow) { $childrenNow } else { "unknown" }), $childrenSeen, $window, (& $show $childRecent))
    Write-Host ("  evidence: a recent figure at or below {0:N2}s reads as idle and at or above {1:N2}s as working; between them this cannot tell." `
                -f $idleFloor, $clearlyBusy)

    if ($proc -and -not $proc.HasExited) {
        if ($everGrew -and $stalledFor -lt $stallLimit) {
            Write-Host ("VERDICT: WORKING -- the log was still growing {0}s before the deadline, so the process was making observable progress when the budget ran out." -f $stalledFor)
            Write-Host ("  The budget was too small for THIS run. #354 records that it has already been raised twice, so this line states what was observed and prescribes nothing.")
        }
        elseif ($null -ne $childRecent -and $childRecent -ge $clearlyBusy) {
            Write-Host ("VERDICT: WORKING -- a CHILD of this process burned {0:N2}s of CPU in the last {1}s. This step spawns a compiler, and a spawned process charges its own CPU, so the parent sitting quiet is expected." -f $childRecent, $window)
            Write-Host ("  The budget was too small for THIS run. #354 records that it has already been raised twice, so this line states what was observed and prescribes nothing.")
        }
        elseif ($null -ne $ownRecent -and $ownRecent -ge $clearlyBusy) {
            Write-Host ("VERDICT: WORKING -- the process burned {0:N2}s of its own CPU in the last {1}s, so it was still running code when the budget expired." -f $ownRecent, $window)
            Write-Host ("  The budget was too small for THIS run. #354 records that it has already been raised twice, so this line states what was observed and prescribes nothing.")
        }
        # The one branch -SelfTest does not stage. Staging it needs a live process
        # whose CPU this script may not read, and every way of arranging that is
        # either platform trivia (a protected system pid, which may or may not
        # refuse an elevated runner) or a test-only seam in production code. It is
        # also the branch that declines to conclude, so an untested one costs a
        # missing answer rather than a wrong one -- which is the direction to be
        # untested in.
        elseif ($null -eq $ownRecent -or $null -eq $childRecent) {
            Write-Host ("VERDICT: INCONCLUSIVE -- the process is alive and quiet, but its CPU could not be sampled ({0} own, {1} descendants), and this wait covers an operation that logs nothing while it runs. Could-not-see is not saw-nothing; do not read this as either a hang or a slow machine." `
                        -f $(if ($null -ne $ownRecent) { "read" } else { "unreadable" }), $(if ($null -ne $childRecent) { "read" } else { "unreadable" }))
        }
        elseif (($ownRecent + $childRecent) -le $idleFloor) {
            Write-Host ("VERDICT: BLOCKED -- the process is alive, wrote nothing for {0}s, and neither it nor any of its {1} descendant(s) consumed measurable CPU in the last {2}s ({3:N2}s against an idle floor of {4:N2}s). Nothing in this process tree was running code when the budget expired." `
                        -f $stalledFor, $childrenSeen, $window, ($ownRecent + $childRecent), $idleFloor)
            Write-Host ("  That is the signature of a hang and not of a slow machine. The log below is where it stopped.")
        }
        else {
            Write-Host ("VERDICT: INCONCLUSIVE -- {0:N2}s of CPU across this process tree in the last {1}s is above the {2:N2}s idle floor and below the {3:N2}s that would settle it. Something ran, and this instrument cannot say whether it was work or a runtime's own housekeeping." `
                        -f ($ownRecent + $childRecent), $window, $idleFloor, $clearlyBusy)
        }
    }
    elseif ($proc) {
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

# ---------------------------------------------------------------------------
# The wait classifier, against synthetic processes.
#
# This exists because the classifier was WRONG in production and nothing noticed:
# it reported "the process was still WORKING (it consumed CPU throughout)" about a
# process that had written nothing for five minutes and spent 1.1% of one core
# (#354). Every other test in this tree passed, because nothing tests a fixture's
# own arithmetic. Five stand-in processes are enough to drive every branch, and
# each one is a shape the real wait genuinely takes:
#
#   a log that keeps growing        -> WORKING, from progress
#   a process spinning on its own   -> WORKING, from its own CPU
#   a quiet parent, spinning child  -> WORKING, from a CHILD's CPU (a spawned `cl`)
#   a process asleep                -> BLOCKED
#   a trickle of CPU and then quiet -> INCONCLUSIVE, which is the honest answer
#   a process that dies             -> EXITED
#
# The fourth is the one that matters: a classifier that cannot be made to say
# BLOCKED cannot report a hang, and the previous one could not.

function Start-Standin([string]$body) {
    # -EncodedCommand rather than -Command: -ArgumentList joins with spaces, so a
    # script body containing any would arrive at the child in pieces. The same trap
    # `ConvertTo-QuotedArgs` above exists for.
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    return Start-Process pwsh -PassThru -WindowStyle Hidden `
                         -ArgumentList '-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded
}

function Wait-Settled($proc, [int]$boundSeconds = 20) {
    # A freshly spawned pwsh burns a few hundred milliseconds starting its runtime,
    # and that must not land inside the window a verdict is drawn from -- it would
    # make the BLOCKED stand-in look busy and the test would pass for the wrong
    # reason. Bounded, and it says what it was waiting for.
    $stop = (Get-Date).AddSeconds($boundSeconds)
    $previous = $null
    while ((Get-Date) -lt $stop) {
        Start-Sleep -Milliseconds 700
        try { $proc.Refresh(); $current = $proc.TotalProcessorTime.TotalSeconds } catch { return }
        if ($null -ne $previous -and ($current - $previous) -lt 0.02) { return }
        $previous = $current
    }
    Write-Host "  note: the stand-in was still consuming CPU after ${boundSeconds}s of settling"
}

function Stop-StandinTree($proc) {
    # Through the same enumeration the classifier uses, so a spinning GRANDCHILD is
    # not left burning a core for three minutes after its case is over.
    $tree = $null
    try { $tree = Get-ProcessTreeCpu $proc.Id $proc.StartTime } catch {}
    if ($tree) {
        foreach ($descendant in $tree.Pids) {
            try { Stop-Process -Id $descendant -Force -ErrorAction Stop } catch {}
        }
    }
    try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {}
}

function Invoke-SelfTest {
    # A literal token rather than the format operator: every stand-in body is
    # PowerShell and is therefore full of braces, and `-f` reads the first `{` it
    # meets as a placeholder and refuses the whole string.
    $logToken = '%LOGPATH%'
    $announce = "Set-Content -LiteralPath '$logToken' -Value 'started'"
    $burnForever = "`$e=[Diagnostics.Stopwatch]::StartNew(); while (`$e.Elapsed.TotalSeconds -lt 180) { `$x=0; foreach (`$i in 1..50000) { `$x+=`$i } }"
    $grandchild = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($burnForever))

    # One row per branch of the verdict. `Expect` is matched as a plain substring --
    # no regex, so the assertion cannot be loosened by an accident of escaping.
    $cases = @(
        @{
            Name   = "a process whose log keeps growing"
            Budget = 6
            Settle = $false
            Body   = "$announce; while (`$true) { Start-Sleep -Milliseconds 250; Add-Content -LiteralPath '$logToken' -Value 'tick' }"
            Expect = "VERDICT: WORKING -- the log was still growing"
            Forbid = @("BLOCKED", "INCONCLUSIVE")
        },
        @{
            Name   = "a process spinning on its own"
            Budget = 10
            Settle = $false
            Body   = "$announce; $burnForever"
            Expect = "VERDICT: WORKING -- the process burned"
            Forbid = @("BLOCKED", "INCONCLUSIVE")
        },
        @{
            Name   = "a quiet parent with a spinning child"
            Budget = 10
            Settle = $true
            Body   = "$announce; Start-Process pwsh -WindowStyle Hidden -ArgumentList '-NoProfile','-EncodedCommand','$grandchild' | Out-Null; Start-Sleep -Seconds 180"
            Expect = "VERDICT: WORKING -- a CHILD of this process burned"
            Forbid = @("BLOCKED", "INCONCLUSIVE")
        },
        @{
            Name   = "a process that is asleep"
            Budget = 10
            Settle = $true
            Body   = "$announce; Start-Sleep -Seconds 180"
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE")
        },
        @{
            # A trickle and then silence is EXACTLY the reading #354 produced, and it
            # is the one the old classifier called WORKING. Neither answer is
            # available from these numbers, so the only correct verdict is that
            # there is no verdict.
            Name   = "a process that burned a little and then went quiet"
            Budget = 10
            Settle = $true
            # The burn is measured by the stand-in against its OWN accounting, so the
            # figure landing in the band is arithmetic rather than a guess about how
            # fast this machine is. `[Diagnostics.Process]` is touched BEFORE the
            # sleep on purpose: its first type load costs about 0.2s, and inside the
            # measured window that alone pushed this case over the working bound and
            # made it report WORKING. Settling absorbs it where it is.
            Body   = "$announce; `$p=[Diagnostics.Process]::GetCurrentProcess(); `$p.Refresh(); Start-Sleep -Seconds 4; `$p.Refresh(); `$t=`$p.TotalProcessorTime.Add([TimeSpan]::FromMilliseconds(250)); while (`$p.TotalProcessorTime -lt `$t) { `$x=0; foreach (`$i in 1..200) { `$x+=`$i }; `$p.Refresh() }; Start-Sleep -Seconds 180"
            Expect = "VERDICT: INCONCLUSIVE"
            Forbid = @("WORKING", "BLOCKED")
        },
        @{
            Name   = "a process that dies instead of hanging"
            Budget = 6
            Settle = $false
            Body   = "exit 3"
            Expect = "the process EXITED with code 3"
            Forbid = @("WORKING", "BLOCKED", "INCONCLUSIVE")
            # The one case that reports no CPU evidence, and deliberately: a process
            # that died did not hang, its exit code is the whole finding, and how
            # much CPU it spent on the way out answers nothing anybody is asking.
            Requires = @()
        }
    )

    # Asserted on EVERY case, not just the ones that tempt it. #354 refuses a third
    # raise of this budget, and the branch that printed that advice was the one that
    # fired most often. A remedy under dispute is not the instrument's to give.
    $forbiddenEverywhere = @("raise the budget", "reduce what runs beside it")

    # What a verdict must SHOW, not just claim. A conclusion whose numbers are not
    # on the page cannot be argued with, and being arguable is the whole value.
    $evidenceLines = @("evidence: own cpu", "evidence: descendants")

    $dir = Join-Path ([IO.Path]::GetTempPath()) ("fc-wait-selftest-" + [Guid]::NewGuid().ToString("N").Substring(0, 12))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    $failures = 0
    $index = 0
    foreach ($case in $cases) {
        $index++
        $log = Join-Path $dir ("case{0}.log" -f $index)
        $proc = Start-Standin ($case.Body.Replace($logToken, $log))
        if ($case.Settle) { Wait-Settled $proc }

        # A pattern nothing writes, so every case reaches the deadline (or dies) and
        # the classifier is what is under test rather than the match.
        $captured = (Wait-ForLogLine $log 'this-line-is-never-written' $case.Budget $case.Name $proc 6>&1 |
                     Where-Object { $_ -isnot [bool] } |
                     ForEach-Object { $_.ToString() }) -join "`n"
        Stop-StandinTree $proc

        $problems = [System.Collections.Generic.List[string]]::new()
        if (-not $captured.Contains($case.Expect)) { $problems.Add("expected `"$($case.Expect)`"") }
        foreach ($banned in @($case.Forbid) + $forbiddenEverywhere) {
            if ($banned -and $captured.Contains($banned)) { $problems.Add("must not say `"$banned`"") }
        }
        # The measurements are the point of the instrument, so their absence is a
        # failure even when the verdict is right. A case may name its own list; the
        # default is that both CPU lines are there.
        $required = if ($case.ContainsKey('Requires')) { @($case.Requires) } else { $evidenceLines }
        foreach ($line in $required) {
            if (-not $captured.Contains($line)) { $problems.Add("expected an `"$line`" line") }
        }

        if ($problems.Count -eq 0) {
            Write-Host ("PASS  {0}" -f $case.Name)
        } else {
            $failures++
            Write-Host ("FAIL  {0}: {1}" -f $case.Name, ($problems -join "; "))
            Write-Host $captured
        }
    }

    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
    if ($failures -gt 0) {
        Write-Host ("node-scratch-isolation-e2e -SelfTest: {0} of {1} cases FAILED" -f $failures, $cases.Count)
        return 1
    }
    Write-Host ("node-scratch-isolation-e2e -SelfTest: {0} cases passed" -f $cases.Count)
    return 0
}

# Before anything with a side effect, so the classifier's own test needs no scratch
# directory, no ports and no binaries.
if ($SelfTest) { exit (Invoke-SelfTest) }

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
