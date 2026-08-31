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

function Get-WaitVerdict($readings) {
    # The DECISION, and nothing else. No process, no clock, no `Get-CimInstance`,
    # no `Get-Content` -- it takes a record of readings and returns the lines a
    # timed-out wait should print, evidence first and verdict last.
    #
    # It is separate from the acquisition below because the first attempt to test
    # it was not: the cases drove real processes, arranged to consume a specific
    # amount of CPU, and read the classifier's answer back. That works for the
    # verdicts that turn on presence or absence and it cannot work for the one
    # that turns on a magnitude. The band between the two bounds is 0.35s wide and
    # a PowerShell process's own interpreter startup costs 0.2-0.5s, so the noise
    # and the signal were the same size. It passed three times locally and failed
    # on `Windows-clangcl-release` at 0.52s against a 0.50s bound; a second
    # arrangement then put the deliberate burn at the recent window's cutoff edge,
    # where only 0.16s of a measured 0.25s counted. Neither was a bound being
    # wrong. There is no stand-in construction that fixes it, because the noise IS
    # the interpreter the stand-in is made of.
    #
    # As a record, every branch is one line and both sides of both bounds can be
    # pinned -- which is where an `-and`/`-or` mistake of the kind this file
    # exists to record actually lives.
    #
    # What deliberately did NOT move: acquisition. `Win32_Process`, the parentage
    # walk, the pid-reuse guard by creation time and the locked-log read are where
    # all three real defects here were, and they are exactly as hard to exercise
    # wherever they live.
    $lines = [System.Collections.Generic.List[string]]::new()
    $show = { param($value) if ($null -ne $value) { "{0:N2}s" -f $value } else { "unknown" } }

    $ownRecent = $readings.OwnRecent
    $childRecent = $readings.ChildRecent
    $window = $readings.Window
    $idleFloor = $readings.IdleFloor
    $clearlyBusy = $readings.ClearlyBusy

    # The measurements first, always, whatever verdict follows: a verdict that turns
    # out wrong is still useful if the numbers it was drawn from are on the page.
    $lines.Add(("  evidence: alive={0} log grew at all={1}, last growth {2}s ago (a stall over {3}s is no longer progress)" `
                -f $readings.Alive, $readings.EverGrew, $readings.StalledFor, $readings.StallLimit))
    $lines.Add(("  evidence: own cpu {0} over the whole wait, {1} in the last {2}s" `
                -f (& $show $readings.OwnTotal), (& $show $ownRecent), $window))
    $lines.Add(("  evidence: descendants now={0}, most seen in the window={1}, their cpu in the last {2}s={3}" `
                -f $(if ($null -ne $readings.ChildrenNow) { $readings.ChildrenNow } else { "unknown" }), `
                   $readings.ChildrenSeen, $window, (& $show $childRecent)))
    # The WHOLE wait, beside the window, and NAMED as a sample rather than as a
    # fact about the wait. This is the one instrument here that has already been
    # misread: "descendants most seen=0" was taken as "no compiler was ever
    # spawned", and a whole hypothesis was declared dead on it.
    #
    # It cannot support that. `Get-ProcessTreeCpu` enumerates processes that are
    # ALIVE at the instant of the sample, so a child that starts and exits
    # between two samples leaves no trace in either the count or the cpu. The
    # probe this fixture waits on is a bare `cl`, measured at 15-36ms warm,
    # against a sampling interval of seconds -- so a zero here is a detection
    # failure and not a negative result, and the second line says so rather than
    # leaving it to be inferred.
    #
    # Raising the rate is not the fix. Nothing cheap enough to run for five
    # minutes catches a 15ms child; what locates a stall is the process saying
    # which phase it finished.
    $lines.Add(("  evidence: over the WHOLE wait: most descendants alive at any one sample={0}, their cpu={1}" `
                -f $(if ($null -ne $readings.ChildrenEver) { $readings.ChildrenEver } else { "unknown" }), `
                   (& $show $readings.ChildTotal)))
    $lines.Add(("  evidence: descendants are SAMPLED every {0}s ({1} samples); a child shorter than that is invisible here, so 0 is not proof that none ran" `
                -f $(if ($null -ne $readings.TreeEvery) { $readings.TreeEvery } else { "?" }), `
                   $(if ($null -ne $readings.TreeSamples) { $readings.TreeSamples } else { "?" })))
    $lines.Add(("  evidence: a recent figure at or below {0:N2}s reads as idle and at or above {1:N2}s as working; between them this cannot tell." `
                -f $idleFloor, $clearlyBusy))

    if (-not $readings.Alive) {
        $lines.Add(("VERDICT: the process exited with code {0} before the deadline." -f $readings.ExitCode))
        return $lines
    }

    if ($readings.EverGrew -and $readings.StalledFor -lt $readings.StallLimit) {
        $lines.Add(("VERDICT: WORKING -- the log was still growing {0}s before the deadline, so the process was making observable progress when the budget ran out." -f $readings.StalledFor))
    }
    elseif ($null -ne $childRecent -and $childRecent -ge $clearlyBusy) {
        $lines.Add(("VERDICT: WORKING -- a CHILD of this process burned {0:N2}s of CPU in the last {1}s. This step spawns a compiler, and a spawned process charges its own CPU, so the parent sitting quiet is expected." -f $childRecent, $window))
    }
    elseif ($null -ne $ownRecent -and $ownRecent -ge $clearlyBusy) {
        $lines.Add(("VERDICT: WORKING -- the process burned {0:N2}s of its own CPU in the last {1}s, so it was still running code when the budget expired." -f $ownRecent, $window))
    }
    elseif ($null -eq $ownRecent -or $null -eq $childRecent) {
        # Asked BEFORE the idle test, and the order is the whole point: a reading
        # that could not be taken must never be folded into a sum and reported as a
        # zero. Could-not-see is not saw-nothing.
        $lines.Add(("VERDICT: INCONCLUSIVE -- the process is alive and quiet, but its CPU could not be sampled ({0} own, {1} descendants), and this wait covers an operation that logs nothing while it runs. Do not read this as either a hang or a slow machine." `
                    -f $(if ($null -ne $ownRecent) { "read" } else { "unreadable" }), $(if ($null -ne $childRecent) { "read" } else { "unreadable" })))
    }
    elseif (($ownRecent + $childRecent) -le $idleFloor) {
        $lines.Add(("VERDICT: BLOCKED -- the process is alive, wrote nothing for {0}s, and neither it nor any of its {1} descendant(s) consumed measurable CPU in the last {2}s ({3:N2}s against an idle floor of {4:N2}s). Nothing in this process tree was running code when the budget expired." `
                    -f $readings.StalledFor, $readings.ChildrenSeen, $window, ($ownRecent + $childRecent), $idleFloor))
        $lines.Add("  That is the signature of a hang and not of a slow machine. The log below is where it stopped.")
    }
    else {
        $lines.Add(("VERDICT: INCONCLUSIVE -- {0:N2}s of CPU across this process tree in the last {1}s is above the {2:N2}s idle floor and below the {3:N2}s that would settle it. Something ran, and this instrument cannot say whether it was work or a runtime's own housekeeping." `
                    -f ($ownRecent + $childRecent), $window, $idleFloor, $clearlyBusy))
    }

    # No verdict above prescribes a remedy. "Raise the budget" is sound advice in
    # general, was printed by the branch that fires most often, and is precisely
    # what #354 refuses -- that budget has been raised twice already. An instrument
    # that prints a remedy has to know when the remedy is under dispute, and this
    # one cannot, so it states the finding and stops.
    return $lines
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
    # This half ACQUIRES; `Get-WaitVerdict` above decides. Everything below reads
    # the world, and nothing below chooses.
    #
    # ------------------------------------------------------------------------
    # What the first version of this classifier got wrong, and why the shape here
    # is the fix rather than a tuning of it (#354's first real sighting):
    #
    #     evidence: alive=True logGrew=True lastGrowth=300s ago cpuDuringWait=3.4s
    #     VERDICT: the process was still WORKING when the budget ran out (it
    #     consumed CPU throughout) -- raise the budget.
    #
    # Three separate errors, and the third is the one worth remembering.
    #
    # 1. `cpuDuringWait` was CUMULATIVE over the whole wait and was read as a claim
    #    about the process's state AT THE DEADLINE. 3.4s spread evenly over 300s and
    #    3.4s burned in the first ten seconds followed by a wedge are the same
    #    number and opposite diagnoses. A duty cycle over that same window does not
    #    fix it -- it is the same number divided by the same 300. Only a RECENT
    #    window can answer a question about now, so that is what the verdict is
    #    drawn from and the totals are evidence only.
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
    # between the fingerprint line and the first registration the node spawns `cl` two
    # or three times and then sits in a kernel wait at zero own-CPU while the child
    # works. Recent-window own-CPU alone would call that BLOCKED, which is error 2
    # again from the other end. The earlier version documented this limitation in
    # prose and told the reader to run `Get-Process cl` by hand; the instrument does
    # it now.
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
    #
    # Accumulated TWICE, over the recent window and over the whole wait, and the
    # second is not decoration. The verdict is drawn from the window and that is
    # correct -- a question about now needs a recent answer. But the EVIDENCE must
    # be able to support the verdict's alternatives, and windowed-only descendant
    # figures cannot: this step spawns the driver two or three times
    # (`CompilerBanner`, then `DiscoverIncludePaths` twice), and a spawn that
    # happened before the window opened is invisible. Twice now that blindness has
    # stood between a reading and a diagnosis -- once as an over-claim that had to
    # be retracted, and once on #354, where it left "did the 300s go into a `cl`
    # spawn or into the in-process walk" unanswerable from a log that had otherwise
    # measured everything needed to say.
    $childRecent = $null
    $childTotal = $null
    $childrenNow = $null
    $childrenSeen = 0
    $childrenEver = 0
    if ($treeSamples.Count -ge 2) {
        $childRecent = 0.0
        $childTotal = 0.0
        foreach ($index in 1..($treeSamples.Count - 1)) {
            $step = $treeSamples[$index].Cpu - $treeSamples[$index - 1].Cpu
            if ($step -gt 0) { $childTotal += $step }
            if ($treeSamples[$index].At -lt $cutoff) { continue }
            if ($step -gt 0) { $childRecent += $step }
        }
        $childrenNow = $treeSamples[$treeSamples.Count - 1].Count
        foreach ($sample in $treeSamples) {
            if ($sample.Count -gt $childrenEver) { $childrenEver = $sample.Count }
            if ($sample.At -ge $cutoff -and $sample.Count -gt $childrenSeen) { $childrenSeen = $sample.Count }
        }
    }

    Write-Host "waited ${seconds}s for $what"
    if ($proc) {
        $readings = [pscustomobject]@{
            Alive        = (-not $proc.HasExited)
            ExitCode     = $(if ($proc.HasExited) { $proc.ExitCode } else { $null })
            EverGrew     = $everGrew
            StalledFor   = $stalledFor
            StallLimit   = $stallLimit
            OwnTotal     = $ownTotal
            OwnRecent    = $ownRecent
            ChildRecent  = $childRecent
            ChildTotal   = $childTotal
            ChildrenNow  = $childrenNow
            ChildrenSeen = $childrenSeen
            ChildrenEver = $childrenEver
            TreeEvery    = $treeEvery.TotalSeconds
            TreeSamples  = $treeSamples.Count
            Window       = $window
            IdleFloor    = $idleFloor
            ClearlyBusy  = $clearlyBusy
        }
        foreach ($line in @(Get-WaitVerdict $readings)) { Write-Host $line }
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
# `Get-WaitVerdict`, against synthesised readings.
#
# This exists because the classifier was WRONG in production and nothing noticed.
# Asked why a worker had not come up in five minutes it answered "the process was
# still WORKING (it consumed CPU throughout)" about a process that had written
# nothing for the whole wait and spent 1.1% of one core, and that verdict carried
# an instruction to raise a budget #354 explicitly refuses. Every other test in
# this tree passed, because nothing tests a fixture's own arithmetic.
#
# The cases are RECORDS rather than processes, and that is the second lesson of
# the same file. The first attempt drove real stand-ins arranged to consume a
# measured amount of CPU. It worked for the verdicts that turn on presence or
# absence and could not work for the one that turns on a magnitude: the band is
# 0.35s wide and a PowerShell process's own startup costs 0.2-0.5s, so the
# instrument's overhead and the quantity under test were the same size. Three
# green runs locally, then 0.52s against a 0.50s bound on CI; and a second
# arrangement put the burn at the recent window's cutoff edge, where 0.16s of a
# measured 0.25s counted. Records take 4ms and cannot drift.
#
# What that buys beyond not flaking:
#
#   - the unsampleable-CPU branch is testable at all, where a live process whose
#     CPU this script may not read could not be arranged;
#   - every bound is pinned on BOTH sides rather than demonstrated once from the
#     middle, which is where an `-and`/`-or` mistake of the original kind lives;
#   - precedence between the branches is assertable, including the one that
#     matters most: an unsampled reading must not be folded into a sum as a zero.
#
# What is NOT covered here, stated rather than left as an apparent omission:
# acquisition. `Win32_Process`, the parentage walk, the pid-reuse guard and the
# locked-log read are where all three real defects were, and they are exactly as
# hard to exercise wherever they live. So is the early return that reports a
# process which died mid-wait; the record below covers only the end-of-wait form.

function Invoke-SelfTest {
    # The reading a healthy-looking but idle process produces: alive, a log that
    # grew long ago, nothing burning. That is BLOCKED, so every case that wants
    # another verdict says exactly which reading changes it -- and a row's
    # overrides are then the whole of what it is about.
    $base = @{
        Alive        = $true
        ExitCode     = $null
        EverGrew     = $true
        StalledFor   = 300
        StallLimit   = 30
        OwnTotal     = 0.0
        OwnRecent    = 0.0
        ChildRecent  = 0.0
        ChildTotal   = 0.0
        ChildrenNow  = 0
        ChildrenSeen = 0
        ChildrenEver = 0
        TreeEvery    = 5
        TreeSamples  = 60
        Window       = 75
        IdleFloor    = 0.15
        ClearlyBusy  = 0.50
    }

    $cases = @(
        @{  Name = "a log still growing at the deadline"
            Readings = @{ StalledFor = 5 }
            Expect = "VERDICT: WORKING -- the log was still growing"
            Forbid = @("BLOCKED", "INCONCLUSIVE") },

        @{  # `-lt`, not `-le`: a stall that has reached the limit is not progress.
            Name = "a log that stopped growing exactly at the stall limit"
            Readings = @{ StalledFor = 30 }
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  # The `-and`. `logGrew` alone was the uninformative half of the
            # original bug: it counted two startup lines landing in different
            # polls. A log that never grew cannot report progress however recently
            # it was read.
            Name = "a log that never grew, however recently it was read"
            Readings = @{ EverGrew = $false; StalledFor = 0 }
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  Name = "a child exactly at the working bound"
            Readings = @{ ChildRecent = 0.50; ChildrenNow = 1; ChildrenSeen = 1 }
            Expect = "VERDICT: WORKING -- a CHILD of this process burned"
            Forbid = @("BLOCKED", "INCONCLUSIVE") },

        @{  Name = "a child just below the working bound"
            Readings = @{ ChildRecent = 0.49; ChildrenNow = 1; ChildrenSeen = 1 }
            Expect = "VERDICT: INCONCLUSIVE"
            Forbid = @("WORKING", "BLOCKED") },

        @{  Name = "the process itself exactly at the working bound"
            Readings = @{ OwnRecent = 0.50 }
            Expect = "VERDICT: WORKING -- the process burned"
            Forbid = @("BLOCKED", "INCONCLUSIVE") },

        @{  Name = "the process itself just below the working bound"
            Readings = @{ OwnRecent = 0.49 }
            Expect = "VERDICT: INCONCLUSIVE"
            Forbid = @("WORKING", "BLOCKED") },

        @{  Name = "own cpu that could not be sampled"
            Readings = @{ OwnRecent = $null }
            Expect = "could not be sampled (unreadable own, read descendants)"
            Forbid = @("WORKING", "BLOCKED") },

        @{  Name = "descendant cpu that could not be sampled"
            Readings = @{ ChildRecent = $null; ChildrenNow = $null }
            Expect = "could not be sampled (read own, unreadable descendants)"
            Forbid = @("WORKING", "BLOCKED") },

        @{  # The precedence that matters most, and the reason the unsampleable
            # test is asked BEFORE the idle one. Folded into a sum, an unreadable
            # reading becomes a zero and a process nobody could see reports as a
            # hang -- the same "could not tell" read as "nothing there" that this
            # repository keeps a list about.
            Name = "an unsampled reading is not a zero"
            Readings = @{ OwnRecent = $null; ChildRecent = 0.0 }
            Expect = "VERDICT: INCONCLUSIVE"
            Forbid = @("WORKING", "BLOCKED") },

        @{  # `-le`, not `-lt`: exactly at the floor is idle.
            Name = "a tree exactly at the idle floor"
            Readings = @{ OwnRecent = 0.15 }
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  Name = "a tree just above the idle floor"
            Readings = @{ OwnRecent = 0.16 }
            Expect = "VERDICT: INCONCLUSIVE"
            Forbid = @("WORKING", "BLOCKED") },

        @{  Name = "a tree at rest"
            Readings = @{}
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  Name = "a busy child outranks an idle parent"
            Readings = @{ OwnRecent = 0.0; ChildRecent = 2.0; ChildrenNow = 1; ChildrenSeen = 1 }
            Expect = "VERDICT: WORKING -- a CHILD of this process burned"
            Forbid = @("BLOCKED", "INCONCLUSIVE") },

        @{  Name = "a growing log outranks an idle tree"
            Readings = @{ StalledFor = 1 }
            Expect = "VERDICT: WORKING -- the log was still growing"
            Forbid = @("BLOCKED", "INCONCLUSIVE") },

        @{  # #354's own reading, as the classifier now receives it. 3.4s of
            # CUMULATIVE cpu with none of it recent, and a log whose only growth
            # was its first two lines. The original called this WORKING and told
            # an operator to raise a budget. It is the case this whole change
            # exists for, and it is one row.
            Name = "the reading that produced #354"
            Readings = @{ EverGrew = $true; StalledFor = 300; OwnTotal = 3.4; OwnRecent = 0.0 }
            Expect = "VERDICT: BLOCKED"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  # #354's real blindness, as a row. A driver spawn that happened before
            # the window opened leaves the windowed descendant figures at zero, and
            # the run then cannot say whether the wait went into a `cl` spawn or
            # into the in-process walk. The whole-wait figures answer that; the
            # VERDICT must not move, because it is still a question about now.
            Name = "a driver spawn before the window is still reported"
            Readings = @{ StalledFor = 300; OwnRecent = 0.0; ChildRecent = 0.0
                          ChildrenNow = 0; ChildrenSeen = 0
                          ChildTotal = 42.0; ChildrenEver = 1 }
            # Asserted on the COUNT and not on the formatted seconds: `{0:N2}` is
            # locale-formatted, so this machine renders "42,00s" and a CI runner
            # renders "42.00s". A case that pinned the decimal separator would
            # pass here and fail there, which is a test asserting the locale.
            Expect = "WHOLE wait: most descendants alive at any one sample=1"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  # And the converse: nothing seen at any sample. This case used to claim
            # that reading "rules a driver spawn out", and it does NOT -- the
            # sampler sees only what is alive when it looks, and the `cl` this
            # fixture waits on runs for 15-36ms warm. Zero here is a detection
            # failure, and #354 had a hypothesis declared dead on it.
            Name = "nothing seen at any sample is not proof that nothing ran"
            Readings = @{ StalledFor = 300; OwnRecent = 0.0; ChildTotal = 0.0; ChildrenEver = 0 }
            Expect = "WHOLE wait: most descendants alive at any one sample=0"
            Forbid = @("WORKING", "INCONCLUSIVE") },

        @{  # The caveat is part of the reading, not a footnote to it: a zero that
            # does not carry its own sampling interval is the sentence that was
            # misread. Pinned on the words that make it unusable as a negative
            # result, so a later tidy-up cannot quietly drop them.
            Name = "a zero descendant count states the interval that could have hidden a child"
            Readings = @{ StalledFor = 300; OwnRecent = 0.0; ChildTotal = 0.0; ChildrenEver = 0 }
            Expect = "is invisible here, so 0 is not proof that none ran"
            Forbid = @() },

        @{  # And the interval itself is the record's, never a literal: a fixture
            # that samples every second and one that samples every five have very
            # different blind spots, and only the number in the record knows which.
            Name = "the sampling interval reported is the one that was used"
            Readings = @{ StalledFor = 300; OwnRecent = 0.0; TreeEvery = 2; TreeSamples = 150 }
            Expect = "SAMPLED every 2s (150 samples)"
            Forbid = @() },

        @{  # And an unsampled whole-wait figure must read as unknown rather than
            # zero, for the reason the windowed one already does: "could not see"
            # folded into "saw nothing" is the failure this file exists about.
            Name = "an unsampled whole-wait figure is not a zero"
            Readings = @{ StalledFor = 300; OwnRecent = 0.0; ChildTotal = $null; ChildrenEver = 2 }
            Expect = "WHOLE wait: most descendants alive at any one sample=2, their cpu=unknown"
            Forbid = @() },

        @{  Name = "a process that died rather than hanging"
            Readings = @{ Alive = $false; ExitCode = 3 }
            Expect = "the process exited with code 3 before the deadline"
            Forbid = @("WORKING", "BLOCKED", "INCONCLUSIVE") }
    )

    # Asserted on EVERY case, not just the ones that tempt it. #354 refuses a third
    # raise of this budget, and the branch that printed that advice was the one that
    # fired most often. A remedy under dispute is not the instrument's to give.
    $forbiddenEverywhere = @("raise the budget", "reduce what runs beside it")

    # What a verdict must SHOW, not just claim. A conclusion whose numbers are not
    # on the page cannot be argued with, and being arguable is the whole value --
    # it is why #354's wrong verdict could be challenged at all.
    $evidenceLines = @("evidence: alive=", "evidence: own cpu", "evidence: descendants",
                       "evidence: over the WHOLE wait", "evidence: descendants are SAMPLED",
                       "evidence: a recent figure")

    $failures = 0
    foreach ($case in $cases) {
        $fields = @{}
        foreach ($key in $base.Keys) { $fields[$key] = $base[$key] }
        foreach ($key in $case.Readings.Keys) { $fields[$key] = $case.Readings[$key] }

        $captured = (@(Get-WaitVerdict ([pscustomobject]$fields))) -join "`n"

        $problems = [System.Collections.Generic.List[string]]::new()
        if (-not $captured.Contains($case.Expect)) { $problems.Add("expected `"$($case.Expect)`"") }
        foreach ($banned in @($case.Forbid) + $forbiddenEverywhere) {
            # Ordinal and case-sensitive, which is load-bearing: the bounds line
            # says "reads as idle and at or above 0.50s as working", and a
            # case-insensitive match would see a forbidden "WORKING" in every
            # single case.
            if ($banned -and $captured.Contains($banned)) { $problems.Add("must not say `"$banned`"") }
        }
        foreach ($line in $evidenceLines) {
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

        # Bound, then surveyed. The first is prompt by construction since #365; the
        # second is the include-tree walk, and it is what the next node must not be
        # started on top of. "serving <compiler> as <fingerprint>" is logged once the
        # survey has an identity -- for a toolchain pinned with `<name>=<compiler>`
        # that is immediate, since an override is never probed.
        function Wait-ForNodeUp([string]$name, $proc, [int]$bindSeconds, [int]$surveySeconds) {
            if (-not (Wait-ForLogLine (Join-Path $phaseDir "$name.err.log") "compile node ready" $bindSeconds "$name to bind its compile port" $proc)) {
                throw "$name did not bind its compile port"
            }
            if (-not (Wait-ForLogLine (Join-Path $phaseDir "$name.err.log") "serving .* as " $surveySeconds "$name to finish its toolchain survey" $proc)) {
                throw "$name did not finish its toolchain survey"
            }
        }

        # The scheduler's own worker serves a fingerprint no client asks for, so every
        # lease has to land on worker A or worker B.
        $schedProc = Start-NodeIn "sched" @(
            "--serve-scheduler", "--listen-node=127.0.0.1:$schedPort", "--fleet-open",
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
        #
        # What that serialisation is asked OF matters, and #365 silently took it
        # away. It used to rest on "compile node ready", which then meant SURVEYED --
        # a node fingerprinted before it bound. Since #365 a node binds and serves
        # first, so that same line means only "bound", every node reached it in about
        # a second, and all three walked the same include tree AT ONCE. The rate
        # measured on the clangcl runner fell to 2-5 file/s against the ~30 file/s
        # #354 measured with one walker, the 600 s budget #428 had just moved here
        # was blown at 5136 files, and an unrelated PR was ejected from the merge
        # queue for it. Waiting on the survey is what the paragraph above always
        # meant; it now has to say so explicitly, because the line it used to rely
        # on no longer carries it.
        #
        # Both waits are kept, and separately: bind and survey are different stages
        # with different costs, and a fixture that folds them cannot say which one
        # stalled.
        Wait-ForNodeUp "sched" $schedProc 180 120

        $workerAProc = Start-NodeIn "workerA" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerA", "--advertise=127.0.0.1:$workerA",
            "--toolchain=$Compiler", "--slots=1") $null
        $procs += $workerAProc
        Wait-ForNodeUp "workerA" $workerAProc 120 600

        $bTemp = if ($separateTempForB) { Join-Path $phaseDir "tempB" } else { $null }
        $workerBProc = Start-NodeIn "workerB" @(
            "--scheduler=127.0.0.1:$schedPort", "--bind=127.0.0.1",
            "--port=$workerB", "--advertise=127.0.0.1:$workerB",
            "--toolchain=$Compiler", "--slots=1") $bTemp
        $procs += $workerBProc
        Wait-ForNodeUp "workerB" $workerBProc 120 600

        # Asked of the SCHEDULER, bounded, and it says what it waited for. A worker
        # logging "compile node ready" says its own port is bound, not that the
        # scheduler has heard from it -- dispatching on that races the first
        # heartbeat and is refused NoWorker.
        #
        # The toolchain walk is NOT in this wait. #428 moved a 600 s budget here on
        # the reasoning that the walk had moved with it, which was half right: the
        # walk did move past the bind, but each node is waited for individually above
        # and is surveyed before the next one starts, so by the time control reaches
        # this line all three identities exist. What remains is the first heartbeat,
        # which is a round trip. A budget sized for the walk would hide a scheduler
        # that never hears a registration behind ten minutes of nothing.
        $registrationBudget = 120
        $deadline = (Get-Date).AddSeconds($registrationBudget); $regs = 0
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
            Write-Host "waited ${registrationBudget}s for three worker registrations at the scheduler; saw $regs (all three nodes had bound their ports AND finished their toolchain surveys, so this covers the first heartbeat only)"
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
