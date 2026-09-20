# SPDX-License-Identifier: Apache-2.0
#
# Per-run TCP ports for the PowerShell e2e fixtures, and what to do about a
# leftover listener -- the PowerShell half of what `free_port` and its
# surrounding argument are for the POSIX fixtures in `e2e-common.sh`.
#
# ## Why this is a module rather than three copies
#
# `#220` fixed ONE fixture. The three helpers below were written correct and left
# private to `run-launcher-e2e.ps1`, so `tls-smoke.ps1` and `run-crossdepth.ps1`
# went on binding a constant and reaping nothing -- #220's defect verbatim, with
# no failure history for the reason `.agent/rules/testing.md` names: a first
# failure MASKS its identical siblings, and `run-launcher-e2e.ps1` was simply the
# one that ran often enough to be observed (#1284).
#
# The POSIX side already paid for the duplication: seven copies of `http_get` and
# `wait_for_port` that had diverged three ways, one with no liveness check at all.
# A helper is shared only if it sits where everything that needs it can include
# from, which for PowerShell is a module next to `e2e-common.sh`.
#
# ## The four properties this must not lose
#
# None of these is new; they are what the private implementation already got
# right, and losing any one makes a shared module worse than the duplication.
#
#   1. A holder is REFUSED, never adopted -- except one whose image path is
#      byte-for-byte the daemon this run would start, which is REAPED.
#   2. A holder whose path could not be READ is refused, or the reap arm kills a
#      process the fixture knows nothing about.
#   3. Ports are drawn from BELOW the kernel's ephemeral range, and remembered.
#   4. The DECISION is what can be wrong, so it is what gets tested --
#      `Invoke-E2EPortSelfTest`, which needs no daemon, no launcher and no
#      compiler.

Set-StrictMode -Version Latest

# The repository root, from this module's own location. Used only by the
# self-test, which drives the consuming fixtures by path.
$script:RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------

# Draw a port nothing is listening on, and remember it.
#
# 20000..32000 for the reason `free_port` in `scripts/lib/e2e-common.sh` gives in
# full: deliberately BELOW the kernel's ephemeral range, because a connect probe
# cannot see a port already held as an OUTBOUND connection's local endpoint. The
# same argument holds on Windows, whose dynamic range starts at 49152.
#
# The probe is a LISTENER bind rather than a connect: binding is what the daemon
# is about to do, so it answers the question actually being asked, and it also
# catches a port held in TIME_WAIT that nothing is listening on.
#
# Issued ports are remembered for the life of the run, because two draws a moment
# apart can both find the same port free -- the first holder has not bound it yet.
# The ledger is module scope, so every fixture in one process shares one.
#
# @return the port.
$script:IssuedPorts = @()
function New-E2EPort {
    for ($attempt = 0; $attempt -lt 200; $attempt++) {
        $candidate = Get-Random -Minimum 20000 -Maximum 32000
        if ($script:IssuedPorts -contains $candidate) { continue }
        $listener = $null
        try {
            $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $candidate)
            $listener.Start()
        } catch {
            continue
        } finally {
            if ($null -ne $listener) { $listener.Stop() }
        }
        $script:IssuedPorts += $candidate
        return $candidate
    }
    throw "could not find a free port in 20000..32000 after 200 attempts"
}

# ---------------------------------------------------------------------------
# The reading
# ---------------------------------------------------------------------------

# What is holding a port, as a RECORD, so the decision below is a pure function
# over it and can be driven without a listener.
#
# `$null` when nothing holds it. Every lookup is best-effort: `Get-NetTCPConnection`
# is absent on some hosts and `Get-Process` fails for a process owned by another
# user, and neither of those means the port is free -- so a failure to NAME the
# holder still reports a holder, with what it could learn.
#
# @param port the TCP port to ask about.
# @return $null, or a record with ProcessId, Name and Path.
function Get-E2EPortHolder([int]$port) {
    # THE BIND PROBE RUNS FIRST, and the ordering is the whole cost of this
    # function. It is the authority and needs no elevation -- so when it SUCCEEDS
    # the port is free and there is no holder to name, which is the common case
    # and the only one that runs in a loop.
    #
    # Measured on this host, 20 iterations each: `Get-NetTCPConnection -LocalPort
    # N -State Listen` is 223 ms warm and 727 ms cold (it autoloads NetTCPIP in a
    # fresh process); the `TcpListener` start/stop is 0.4 ms. **550x.** Asking the
    # cmdlet first put 223 ms inside every iteration of the reap wait below, whose
    # `50 x 100 ms` reads as a 5 s bound and really took ~16 s -- so the refusal
    # arrived about eleven seconds after the moment it claims to describe. A bound
    # nobody has measured is not a bound.
    $listener = $null
    try {
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
        $listener.Start()
        return $null
    } catch {
        # Something holds it. NOW it is worth asking who.
    } finally {
        if ($null -ne $listener) { $listener.Stop() }
    }

    # Best-effort from here down: `Get-NetTCPConnection` is absent on some hosts
    # and `Get-Process` fails for a process owned by another user, and neither of
    # those means the port is free -- so a failure to NAME the holder still
    # reports a holder, with what it could learn.
    $owner = $null
    try {
        $owner = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction Stop |
                 Select-Object -First 1 -ExpandProperty OwningProcess
    } catch {
    }
    if ($null -eq $owner) { return @{ ProcessId = 0; Name = "(unknown)"; Path = "" } }

    $name = "(unknown)"
    $path = ""
    try {
        $proc = Get-Process -Id $owner -ErrorAction Stop
        $name = $proc.ProcessName
        if ($proc.Path) { $path = $proc.Path }
    } catch {
    }
    return @{ ProcessId = [int]$owner; Name = $name; Path = $path }
}

# ---------------------------------------------------------------------------
# The decision
# ---------------------------------------------------------------------------

# Pure over the record above. THREE outcomes, never two.
#
# `reap` is for a leftover of THIS build -- a run that did not reach its cleanup --
# and it is a reap rather than an adoption, which is the distinction #220 asks to
# have made: adopting a stale listener is tempting and wrong, because it is of an
# unknown vintage and may hold a store from a different build, which is the class
# of confusion these fixtures exist to DETECT rather than reproduce. Killing one
# whose image path is byte-for-byte the daemon we are about to start is not
# adoption; it is the manual `Stop-Process` somebody already does.
#
# Anything else is `refuse`, by name. A port some other program holds is not a
# fixture's to take.
#
# @param holder The record from Get-E2EPortHolder, or $null.
# @param expectedPath The daemon this run would start, ABSOLUTE -- see
#        `Resolve-E2EImagePath`, which is what the callers below put it through.
# @return 'free', 'reap' or 'refuse'.
function Resolve-E2EPortHolder($holder, [string]$expectedPath) {
    if ($null -eq $holder) { return 'free' }
    if ($holder.Path -and $expectedPath -and
        ([string]::Equals($holder.Path, $expectedPath, [StringComparison]::OrdinalIgnoreCase))) {
        return 'reap'
    }
    return 'refuse'
}

# The expected image path, as a holder's would be spelled.
#
# `Get-Process().Path` is always absolute and normalised, so an expected path that
# is relative or carries `..` segments can never equal one -- which silently turns
# `reap` into `refuse` for a fixture invoked the way `run-crossdepth.ps1` defaults
# to invoking itself (`$PSScriptRoot/../../out/build/...`). That is the SAFE
# direction, so it never showed up as a failure; it showed up as a reaper that
# could not fire.
#
# A path that cannot be resolved is returned unchanged rather than guessed at,
# which keeps the failure in that same safe direction.
#
# @param path a path as a caller spelled it.
# @return the same path, absolute where that could be established.
function Resolve-E2EImagePath([string]$path) {
    if (-not $path) { return $path }
    try {
        return (Resolve-Path -LiteralPath $path -ErrorAction Stop).ProviderPath
    } catch {
        return $path
    }
}

# The refusal, as ONE template both the thrower and its test read.
#
# ## Why a template rather than a literal at each end
#
# The self-test drives the consuming fixtures as CHILD PROCESSES, so the only
# thing it can read back is this sentence. A test matching a phrase it REMEMBERS
# stops testing the day either end is reworded, and the two would be reworded
# apart -- so the expected message is built from the same template the thrower
# uses, with the same holder record.
#
# ## And never a `+` around a `-f`
#
# PowerShell binds `-f` TIGHTER than `+`, so
#
#     ("...{0}..." + "...no placeholders..." -f $a, $b)
#
# formats the SECOND fragment -- which has no placeholders, so it comes back
# unchanged -- and concatenates the first one with its `{0} {1} {2}` still in it.
# That is what this refusal did: it named the port and then said `held by {0}
# (pid {1}, {2})`, which is #220's own defect reappearing inside #220's own fix.
# The case covering it asserted only that a throw happened -- something BOTH
# states produce -- so it was green from the day it was written.
$script:E2ERefusalTemplate =
    "port {0} is already held by {1} (pid {2}, {3}); this fixture will not take a port another program is using, and will not adopt a listener of unknown vintage"

# How many `a + b -f args` expressions a script contains -- a concatenation whose
# right-hand fragment is then formatted, when the author meant to format the whole
# thing.
#
# PowerShell binds `-f` TIGHTER than `+`. Measured on 7.6.6:
#
#     "a {0}" + "b" -f 1        ->  a {0}b
#
# so the format runs on `"b"`, which has no placeholders and comes back unchanged,
# and the left fragment keeps its `{0}` literally. It is not a syntax error and
# not a runtime error; it is a sentence that still looks like a sentence, which is
# why it survived in `Assert-E2EPortAvailable` from the day #220 was fixed and was
# then written AGAIN, in this file, by the author of this paragraph.
#
# ## The node shape is the opposite of the reading
#
# The tempting predicate -- a `Format` node whose left operand is a `Plus` -- is
# the precedence read as it is spoken, and it matches NOTHING. Tighter binding
# means the `Format` is the INNER node: the tree is `Plus(left, Format(b, args))`.
# The first version of this guard used the spoken shape, reported the module clean,
# and was caught only by its positive control -- which is the whole argument for
# having one.
#
# A deliberately parenthesised `"a" + ("b" -f 1)` is NOT matched: the parentheses
# make the right operand a `ParenExpressionAst`, so saying what you mean still
# works.
#
# @param path a script to read, or
# @param text the source directly, for the positive control.
# @return the number of occurrences.
function Measure-E2EFormatPrecedence([string]$path = "", [string]$text = "") {
    $errors = $null
    $ast = if ($text) {
        [System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$null, [ref]$errors)
    } else {
        [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors)
    }
    $found = $ast.FindAll({
        param($node)
        ($node -is [System.Management.Automation.Language.BinaryExpressionAst]) -and
        ($node.Operator -eq [System.Management.Automation.Language.TokenKind]::Plus) -and
        ($node.Right -is [System.Management.Automation.Language.BinaryExpressionAst]) -and
        ($node.Right.Operator -eq [System.Management.Automation.Language.TokenKind]::Format)
    }, $true)
    return @($found).Count
}

# @param port the port that was refused.
# @param holder the record from Get-E2EPortHolder.
# @return the refusal sentence.
function Get-E2EPortRefusal([int]$port, $holder) {
    $where = if ($holder.Path) { $holder.Path } else { "path unknown" }
    return ($script:E2ERefusalTemplate -f $port, $holder.Name, $holder.ProcessId, $where)
}

# The pre-flight. Only reached for an explicitly passed port, since a drawn one
# was proved free a moment ago.
#
# The message NAMES the port and the process, which is the cheapest half of #220:
# `fastcached exited immediately (exit 1)` named neither, read exactly like a
# daemon that cannot start, and cost a twenty-minute diagnosis by hand.
#
# @param port the port the caller chose.
# @param expectedPath the daemon this run would start.
function Assert-E2EPortAvailable([int]$port, [string]$expectedPath) {
    $expected = Resolve-E2EImagePath $expectedPath
    $holder = Get-E2EPortHolder $port
    switch (Resolve-E2EPortHolder $holder $expected) {
        'free' { return }
        'reap' {
            Write-Host ("port ${port} is held by a leftover {0} (pid {1}) from this build; reaping it" -f `
                        $holder.Name, $holder.ProcessId)
            Stop-Process -Id $holder.ProcessId -Force -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt 50; $i++) {
                if ($null -eq (Get-E2EPortHolder $port)) { return }
                Start-Sleep -Milliseconds 100
            }
            throw ("port ${port} is still held after reaping pid {0}" -f $holder.ProcessId)
        }
        'refuse' { throw (Get-E2EPortRefusal $port $holder) }
    }
}

# Draw a port, or pre-flight the one the caller chose. The one entry point a
# fixture needs, so the two-arm shape cannot be half-copied.
#
# @param port 0 to draw, or the port the caller chose.
# @param expectedPath the daemon this run would start.
# @param label what to call the port in the line this prints.
# @return the port to use.
function Get-E2EFixturePort([int]$port, [string]$expectedPath, [string]$label = "fastcached") {
    if ($port -eq 0) {
        $drawn = New-E2EPort
        Write-Host "${label} port for this run: ${drawn}"
        return $drawn
    }
    # The caller chose the collision risk, so the collision is checked for.
    Assert-E2EPortAvailable $port $expectedPath
    Write-Host "${label} port (given): ${port}"
    return $port
}

# ---------------------------------------------------------------------------
# Readiness
# ---------------------------------------------------------------------------

# Wait until something is answering on a port, bounded, saying what it waited for
# and WHICH KIND of failure a timeout was.
#
# A flat `Start-Sleep` is neither. It is either flaky on a cold runner or slow on
# a warm one, and when it is too short the failure surfaces one step later as the
# client being unable to connect -- which is a true observation carrying a false
# subject. Measured while converting `tls-smoke.ps1`: an 800 ms sleep in front of
# a daemon that had ALREADY EXITED reported
# `No connection could be made because the target machine actively refused it`,
# sending a reader to the network for a process that printed its own reason and
# stopped.
#
# So a process that DIED is reported the moment it is noticed rather than after
# the budget: it is a third case beside slow and stuck, calling it a timeout sends
# the reader to the budget, and the budget is not the subject.
#
# @param port the port to poll.
# @param process the Start-Process object to watch, or $null when there is none.
# @param what what it is, for the messages.
# @param logs any files to dump on a failure.
# @param seconds the bound; 20 by default.
function Wait-E2EPortAnswers([int]$port, $process, [string]$what, [string[]]$logs = @(), [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    $polls = 0
    while ((Get-Date) -lt $deadline) {
        if ($null -ne $process -and $process.HasExited) {
            Write-E2ELogs $logs
            $code = $process.ExitCode
            throw ("${what} EXITED while we waited for it to answer on ${port}: exit code ${code}. " +
                   "That is not a timeout and the budget is not the subject.")
        }
        $client = $null
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $port)
            Write-Host "${what} answered on ${port} after ${polls} poll(s)"
            return
        } catch {
        } finally {
            if ($null -ne $client) { $client.Dispose() }
        }
        $polls++
        Start-Sleep -Milliseconds 100
    }
    Write-E2ELogs $logs
    throw ("waited ${seconds}s over ${polls} poll(s) for ${what} to answer on ${port}. " +
           "It is still running, so this is a slow machine or a process that never bound.")
}

# Read a file a LIVE process is still writing to.
#
# `Get-Content -Raw` is the obvious spelling and it is the wrong one here. On
# Windows, opening a file another process holds fails with a sharing violation
# depending on the FileShare mode that process opened it with -- and every caller
# below is dumping a `-RedirectStandardOutput` target of a daemon that is STILL
# RUNNING, which is the timeout arm's whole situation. Inside a swallowing `catch`
# that prints NOTHING, so the failure message loses the daemon's own reason, which
# is the reason the wait was converted in the first place.
#
# `FileShare.ReadWrite` says explicitly that a concurrent writer is expected. The
# same argument, and the same implementation, as `Read-LiveText` in
# `scripts/dist-compile-e2e.ps1`, which met it first.
#
# Empty rather than throwing when the file is missing or momentarily unreadable:
# both are ordinary on a failure path.
#
# @param path the file to read.
# @return its text, or "".
function Read-E2ELiveText([string]$path) {
    if (-not $path) { return "" }
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

# Dump whatever a failed wait can offer. Every read is best-effort: a diagnostic
# on an already-failed case must be able to EXPLAIN the verdict and never change
# it.
#
# @param logs the files to print.
function Write-E2ELogs([string[]]$logs) {
    foreach ($log in $logs) {
        if (-not $log) { continue }
        if (-not (Test-Path $log)) { continue }
        Write-Host "--- ${log} ---"
        Write-Host (Read-E2ELiveText $log)
    }
}

# ---------------------------------------------------------------------------
# The consumers, as a table
# ---------------------------------------------------------------------------
#
# Every PowerShell fixture that binds a port it chose. A row is what makes the
# site proof below possible at all: the self-test holds a listener on the port,
# runs the fixture, and requires it to refuse by name.
#
# Adding a port-binding PowerShell fixture is adding a row. A row whose file does
# not import this module is REFUSED, which is the half that keeps the table from
# describing a fixture that has quietly gone back to a constant.
#
# `Stage` names the files the fixture must find before it reaches the pre-flight,
# staged EMPTY. That ordering is itself asserted: a fixture whose pre-flight ran
# ahead of its own prerequisite guards would report a missing binary as a port
# collision, and this table would never notice because the refusal it asserts is
# the same one.
$script:E2EPortConsumers = @(
    @{
        Name  = 'tls-smoke.ps1'
        Path  = 'scripts/tls-smoke.ps1'
        Stage = @('fastcached.exe', 'server.crt', 'server.key')
        Argv  = {
            param($stage, $port)
            @('-Fastcached', (Join-Path $stage 'fastcached.exe'),
              '-Cert',       (Join-Path $stage 'server.crt'),
              '-Key',        (Join-Path $stage 'server.key'),
              '-Port',       $port)
        }
    }
    @{
        Name  = 'run-crossdepth.ps1'
        Path  = 'src/apps/compile-cache-testclient/run-crossdepth.ps1'
        Stage = @('fastcached.exe', 'compile-cache-testclient.exe')
        Argv  = {
            param($stage, $port)
            @('-Fastcached', (Join-Path $stage 'fastcached.exe'),
              '-Client',     (Join-Path $stage 'compile-cache-testclient.exe'),
              '-Port',       $port,
              '-Synthetic')
        }
    }
    @{
        Name  = 'run-launcher-e2e.ps1'
        Path  = 'src/apps/fastcache-cc/run-launcher-e2e.ps1'
        Stage = @('fastcached.exe', 'fastcache-cc.exe')
        Argv  = {
            param($stage, $port)
            @('-Fastcached', (Join-Path $stage 'fastcached.exe'),
              '-Launcher',   (Join-Path $stage 'fastcache-cc.exe'),
              '-Port',       $port)
        }
    }
    @{
        Name  = 'sccache-smoke.ps1'
        Path  = 'scripts/sccache-smoke.ps1'
        Stage = @('fastcached.exe')
        # The ONE row that may legitimately answer 77, and it says WHICH 77.
        #
        # A bare `$true` accepts any of this fixture's four skip exits -- sccache
        # absent, fastcached absent, compiler absent, backend not compiled in --
        # and two of those are staged away by the `Stage` list and the `--compiler`
        # argument above. So if that bookkeeping ever drifts, a `$true` row
        # degrades to a PERMANENT SILENT SKIP, which is the one outcome the column
        # exists to prevent, reinstated for the row that has it. The reason is a
        # pattern matched against what the fixture SAID.
        MaySkip = 'sccache not found|built without the .* backend'
        Argv  = {
            param($stage, $port)
            # `--compiler` names THIS interpreter, which certainly exists. The
            # fixture only asks whether the compiler is resolvable, and it does so
            # BEFORE the port pre-flight; it never spawns one here, because the
            # pre-flight refuses first. Passing a real compiler name would make
            # this row skip on every host without `cl` -- a case that did not run,
            # reported as a host property rather than as the coverage gap it is.
            @('--fastcached', (Join-Path $stage 'fastcached.exe'),
              '--protocol',   'memcached',
              '--compiler',   (Get-Process -Id $PID).Path,
              '--port',       $port)
        }
    }
)

# ---------------------------------------------------------------------------
# The self-test
# ---------------------------------------------------------------------------

# Drive the decision and the acquisition, and prove the reaper BITES at every
# consuming site.
#
# Needs no daemon, no launcher and no compiler -- which is the point, and is what
# `.agent/rules/testing.md` records about the registration: the decision is what
# can be wrong, and it was previously reachable only by running a fixture that
# needs an MSVC toolchain, so on every machine and every CI leg without one it was
# asserted by nothing.
#
# One case. At MODULE scope rather than nested inside the self-test, because
# `Invoke-E2EPortSiteCases` calls it too: PowerShell would resolve a nested
# definition through the dynamic scope chain and it would work, which is exactly
# the kind of working nobody can read.
#
# @param what the case, as a sentence.
# @param want the expected value.
# @param got the observed value.
$script:SelfTestCases = 0
$script:SelfTestBad = 0
function Expect([string]$what, $want, $got) {
    $script:SelfTestCases++
    if ("$want" -eq "$got") {
        Write-Host "  ok   $what"
    } else {
        Write-Host "  FAIL ${what}: want [$want] got [$got]"
        $script:SelfTestBad++
    }
}

# @return 0 when every case held, 1 otherwise.
function Invoke-E2EPortSelfTest {
    $script:SelfTestCases = 0
    $script:SelfTestBad = 0

    Write-Host "e2e port helpers self-test"

    # The DECISION, every arm, over staged records -- no listener needed. The
    # ACCEPTING arm first and deliberately: a `Resolve-E2EPortHolder` that answered
    # `refuse` unconditionally would satisfy every refusing case below while
    # refusing every honest run.
    Expect "nothing listening is free" "free" (Resolve-E2EPortHolder $null "C:\build\fastcached.exe")
    Expect "our own daemon is reaped, not adopted" "reap" `
        (Resolve-E2EPortHolder @{ ProcessId = 1; Name = "fastcached"; Path = "C:\build\fastcached.exe" } `
                               "C:\build\fastcached.exe")
    Expect "and case-insensitively, because Windows paths are" "reap" `
        (Resolve-E2EPortHolder @{ ProcessId = 1; Name = "fastcached"; Path = "C:\Build\FastCached.EXE" } `
                               "c:\build\fastcached.exe")
    Expect "another build of the same daemon is REFUSED, not adopted" "refuse" `
        (Resolve-E2EPortHolder @{ ProcessId = 2; Name = "fastcached"; Path = "D:\other\fastcached.exe" } `
                               "C:\build\fastcached.exe")
    Expect "somebody else entirely is refused" "refuse" `
        (Resolve-E2EPortHolder @{ ProcessId = 3; Name = "sqlservr"; Path = "C:\sql\sqlservr.exe" } `
                               "C:\build\fastcached.exe")
    # A holder whose path could not be read is REFUSED. It is the direction that
    # matters: an unreadable path reaching `reap` would kill a process this fixture
    # knows nothing about.
    Expect "a holder we cannot name is refused, never reaped" "refuse" `
        (Resolve-E2EPortHolder @{ ProcessId = 0; Name = "(unknown)"; Path = "" } "C:\build\fastcached.exe")

    # THE RENDERED REFUSAL, on its own, and it needs its own cases for a reason
    # that is not obvious: the site cases below build their expected sentence from
    # the same template the thrower uses, so a template that renders WRONGLY
    # renders wrongly at both ends and they agree perfectly. Measured -- restoring
    # the `+`-around-`-f` defect left all three site cases green.
    #
    # So the render is asserted against the FACTS, computed here and not taken
    # from the template. The unsubstituted-placeholder case states the defect
    # class directly, because that is what it looked like: a sentence of the right
    # length, in the right place, with `{0} (pid {1}, {2})` where the holder
    # should have been.
    $renderHolder = @{ ProcessId = 4242; Name = "sqlservr"; Path = "C:\sql\sqlservr.exe" }
    $rendered = Get-E2EPortRefusal 24680 $renderHolder
    Expect "the refusal names the port" $true $rendered.Contains("24680")
    Expect "the refusal names the holder" $true $rendered.Contains("sqlservr")
    Expect "the refusal names the holder's pid" $true $rendered.Contains("4242")
    Expect "the refusal names the holder's image" $true $rendered.Contains("C:\sql\sqlservr.exe")
    Expect "the refusal substitutes every placeholder" $false ($rendered -match '\{\d+\}')
    # A holder with no readable path says so rather than trailing off.
    $anon = Get-E2EPortRefusal 24680 @{ ProcessId = 0; Name = "(unknown)"; Path = "" }
    Expect "an unnamed holder's refusal says the path is unknown" $true $anon.Contains("path unknown")
    Expect "and it substitutes every placeholder too" $false ($anon -match '\{\d+\}')

    # And the SHAPE, over this module's own source, because knowing the rule is
    # not protection: the bounded wait added in the same change reproduced it
    # within the hour, one screen below the paragraph explaining it, and produced
    # `exit code {0}` for a daemon that had died.
    #
    # Asked of the PARSE TREE rather than of the text. A regex for `+` near `-f`
    # is approximate in both directions -- it cannot see a concatenation split
    # across a line continuation, and it fires on either token appearing in a
    # comment -- while `(a + b) -f args` is exactly one node shape.
    # EVERY tracked PowerShell file, not just this one. The hazard is a property
    # of the LANGUAGE, and its history is that it was written twice inside one
    # change by one author -- so scoping the guard to the file where it happened
    # to be noticed is scoping it to the one place it has already been fixed.
    $psFiles = @(Get-ChildItem -Path $script:RepoRoot -Recurse -File -Include *.ps1, *.psm1 `
                     -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notmatch '[\\/](out|vendor|\.git)[\\/]' })
    $offenders = @()
    foreach ($f in $psFiles) {
        if ((Measure-E2EFormatPrecedence $f.FullName) -gt 0) { $offenders += $f.Name }
    }
    # A walk that matched nothing reports every file clean, which reads exactly
    # like complete coverage.
    Expect "the PowerShell walk found files to read" $true ($psFiles.Count -ge 5)
    Expect "no tracked PowerShell file concatenates in front of -f" "" ($offenders -join ', ')

    # AND NO SIXTH PRIVATE PORT DRAW. Nothing in PowerShell makes a fixture reach
    # for `New-E2EPort`, and the next author will write whatever looks right --
    # which is how five copies accumulated and how one of them came to draw
    # 20000..30000 while the others drew 20000..32000. Same shape as the scan
    # `.agent/rules/testing.md` records for `UniqueScratchPath`: the fix is a
    # seam, so the guard is a scan.
    #
    # A numeric `Get-Random -Minimum <4-5 digits>` IS a port draw here; nothing
    # else in this tree draws a number in that band. Each exemption carries its
    # reason, because an exemption spelled like a forgotten row is the thing this
    # repository refuses.
    $drawExempt = @{
        'E2EPorts.psm1' = 'the module itself -- this is the draw every other file is meant to call'
        'dist-compile-e2e.ps1' = 'Get-FreePortBlock draws CONSECUTIVE ports with a CONNECT probe, which New-E2EPort cannot stand in for'
    }
    $drawOffenders = @()
    foreach ($f in $psFiles) {
        if ((Read-E2ELiveText $f.FullName) -notmatch 'Get-Random\s+-Minimum\s+\d{4,5}') { continue }
        if ($drawExempt.ContainsKey($f.Name)) { continue }
        $drawOffenders += $f.Name
    }
    Expect "no tracked PowerShell file draws its own port" "" ($drawOffenders -join ', ')
    # An exemption that has stopped describing a draw is a licence outliving its
    # argument, so a row naming a file that no longer draws is refused.
    $staleExempt = @()
    foreach ($name in $drawExempt.Keys) {
        $f = $psFiles | Where-Object { $_.Name -eq $name } | Select-Object -First 1
        if ($null -eq $f) { $staleExempt += "${name} (no such file)"; continue }
        if ((Read-E2ELiveText $f.FullName) -notmatch 'Get-Random\s+-Minimum\s+\d{4,5}') {
            $staleExempt += "${name} (no longer draws a port)"
        }
    }
    Expect "every draw exemption still describes a draw" "" ($staleExempt -join ', ')
    # A guard nobody has watched REFUSE is not known to work, and this one was
    # BACKWARDS on its first writing: it reported the module clean while matching
    # nothing at all, and only this staged line said so.
    Expect "and that guard finds the shape when it is there" 1 `
        (Measure-E2EFormatPrecedence -text '$x = "a {0}" + "b" -f 1')
    # Saying what you mean is still allowed, so the guard must not fire on it --
    # otherwise the remedy it implies is unavailable and somebody deletes it.
    Expect "and not on a parenthesised format" 0 `
        (Measure-E2EFormatPrecedence -text '$x = "a" + ("b {0}" -f 1)')

    # The expected path is NORMALISED before the comparison, or the reap arm can
    # never fire for a fixture invoked with a relative path -- which two of the
    # three rows above default to. Driven over this file, which certainly exists,
    # so the assertion is about normalisation rather than about resolution failing.
    $selfDir = $PSScriptRoot
    $viaDots = Join-Path (Join-Path $selfDir '..') 'lib'
    Expect "a path with .. segments resolves to the same file" `
        (Resolve-E2EImagePath $selfDir) (Resolve-E2EImagePath $viaDots)
    # And one that cannot be resolved comes back unchanged rather than guessed at,
    # which keeps an unresolvable expectation in the refusing direction.
    Expect "an unresolvable path is returned as it was given" `
        "C:\no\such\daemon.exe" (Resolve-E2EImagePath "C:\no\such\daemon.exe")

    # And the ACQUISITION, against real sockets on this machine.
    $drawn = @(1..5 | ForEach-Object { New-E2EPort })
    Expect "five draws are five distinct ports" 5 (($drawn | Select-Object -Unique).Count)
    $inRange = @($drawn | Where-Object { $_ -ge 20000 -and $_ -lt 32000 }).Count
    Expect "every draw is below the ephemeral range" 5 $inRange

    # A real listener, so the probe is measured rather than argued. `Get-E2EPortHolder`
    # must report SOMETHING for a port that is held -- which is the half that decides
    # whether the pre-flight can fire at all.
    $held = New-E2EPort
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $held)
    $listener.Start()
    try {
        $holder = Get-E2EPortHolder $held
        Expect "a held port reports a holder" $true ($null -ne $holder)
        Expect "and an unheld one reports none" $true ($null -eq (Get-E2EPortHolder (New-E2EPort)))
        # The whole point, end to end: a taken port refuses by name rather than
        # letting the daemon start and fail with a sentence naming neither.
        $refused = $false
        try { Assert-E2EPortAvailable $held "C:\nothing\fastcached.exe" } catch { $refused = $true }
        Expect "an occupied port is refused before the daemon starts" $true $refused

        # THE SITES. A fixture that never meets a holder passes identically with
        # the reaper deleted, which is what #220 shipped and #1284 is about: every
        # consuming fixture is driven with the port already held and must refuse.
        Invoke-E2EPortSiteCases $held
    } finally {
        $listener.Stop()
    }

    # A self-test that ran nothing is not a self-test that passed. Zero cases and
    # a clean tree produce byte-identical output otherwise.
    if ($script:SelfTestCases -lt 1) {
        Write-Host "e2e port helpers self-test FAILED: it ran no cases at all"
        return 1
    }
    if ($script:SelfTestBad -ne 0) {
        Write-Host "e2e port helpers self-test FAILED after $($script:SelfTestCases) case(s)"
        return 1
    }
    Write-Host "e2e port helpers self-test passed, $($script:SelfTestCases) case(s)"
    return 0
}

# Every consuming fixture, driven against a port this process is holding.
#
# Three claims per row, and the third is the only one that DISCRIMINATES:
#
#   * the file IMPORTS this module -- a scan, because a fixture that went back to
#     its own constant would still refuse nothing and this table would go on
#     describing it;
#   * it exits non-zero and NOT 77, because a skip is what a missing prerequisite
#     looks like and reading one as a refusal would pass this case for a fixture
#     whose pre-flight never ran;
#   * it printed THE REFUSAL -- the exact sentence, holder and all.
#
# The third is what the first two cannot do, and that was measured rather than
# reasoned: with the decision neutered to `free` every fixture still exited
# non-zero (it went on to start a staged empty file as a daemon) and still printed
# the port (the ACCEPTING arm announces it too), so both of the first two claims
# passed under the bug. They are what BOTH sides produce.
#
# ## The output is FLATTENED before it is matched
#
# PowerShell wraps an exception at the console width, so the sentence arrives
# split across lines at a column nobody chose. Matching it unflattened is a test
# whose verdict depends on the terminal -- the same trap CMake's wrapped
# diagnostics set for this repository's `cmake -P` readers.
#
# ## And the expected sentence is built from OUR OWN reading of the holder
#
# Both ends ask the same host about the same listener, so a host that cannot
# attribute one produces `(unknown) (pid 0, path unknown)` at both ends and the
# comparison still holds. That is reported rather than silently accepted: on such
# a host this case proves the refusal fired and does NOT prove a holder was named.
#
# @param held a port this process is currently listening on.
function Invoke-E2EPortSiteCases([int]$held) {
    $pwshPath = (Get-Process -Id $PID).Path
    if (-not $pwshPath) {
        Write-Host "  SKIP the site cases: this process cannot name its own image path,"
        Write-Host "       so a fixture driven here could not be handed a staged daemon to compare against"
        return
    }

    # Can this host attribute a listener to a process at all? Asked of our OWN
    # listener, which we know the answer for, so the arm below is selected by what
    # was OBSERVED rather than by a platform name.
    $ourHolder = Get-E2EPortHolder $held
    if ($null -eq $ourHolder) {
        Write-Host "  SKIP the site cases: this process is listening on ${held} and cannot see it,"
        Write-Host "       so there is no holder for a fixture driven here to refuse"
        return
    }
    if ($ourHolder.ProcessId -eq 0) {
        Write-Host "  note: this host cannot attribute a listener to a process, so the site cases below"
        Write-Host "        prove the refusal FIRED and do not prove a holding process was named"
    }
    # Flattened, because a thrown message wraps at the console width. Built from
    # the same template the thrower uses, and from the same reading of the holder.
    $wantRefusal = (Get-E2EPortRefusal $held $ourHolder) -replace '\s+', ' '

    $stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("e2e-ports-" + [System.IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
    try {
        foreach ($row in $script:E2EPortConsumers) {
            $scriptPath = Join-Path $script:RepoRoot $row.Path
            if (-not (Test-Path $scriptPath)) {
                Expect "$($row.Name) exists at the path this table names" $true $false
                continue
            }

            # It draws through this module, rather than through a copy or a constant.
            $text = Get-Content -Raw $scriptPath
            Expect "$($row.Name) imports the shared port module" $true ($text -match 'E2EPorts\.psm1')

            # STAGED, AND THEN ASSERTED. A fixture that quietly tested a different
            # state than it believes it created is the failure mode that reads as
            # a pass -- and here it would read as the WRONG failure: a staging
            # that silently did nothing makes the fixture exit 77 on a missing
            # binary, and the case below would report "refuses a held port with a
            # failure, not a skip" while naming a subject that was never reached.
            $missing = @()
            foreach ($leaf in $row.Stage) {
                $staged = Join-Path $stageRoot $leaf
                if (-not (Test-Path $staged)) { New-Item -ItemType File -Path $staged | Out-Null }
                if (-not (Test-Path $staged)) { $missing += $leaf }
            }
            Expect "$($row.Name)'s prerequisites were staged" "" ($missing -join ', ')
            if ($missing.Count -gt 0) { continue }

            $argv = & $row.Argv $stageRoot $held
            # This process's own interpreter, never a bare `pwsh` off PATH: the
            # ctest registration invokes us by full path precisely because PATH
            # need not carry one, and a child that could not START would present
            # as a fixture that failed to refuse.
            $output = (& $pwshPath -NoProfile -File $scriptPath @argv 2>&1 | Out-String)
            $code = $LASTEXITCODE

            # 77 is a SKIP -- a missing runtime prerequisite -- and which rows may
            # answer it is a COLUMN, never a blanket tolerance. A row that may not
            # skip and does has moved its pre-flight ABOVE its own guards, which is
            # a real defect this case exists to catch; swallowing 77 everywhere
            # would report that as a clean refusal. A row that may skip and does is
            # reported as SKIPPED with the fixture's own reason, because absent and
            # skipped are two states and neither is a pass.
            #
            # The column is the REASON, so a 77 this row did not predict still
            # fails it. See the row's own comment for why a boolean is not enough.
            $maySkip = $row.ContainsKey('MaySkip') -and $row.MaySkip -and
                       (($output -replace '\s+', ' ') -match $row.MaySkip)
            if ($maySkip -and $code -eq 77) {
                Write-Host "  SKIP $($row.Name): it reported a missing prerequisite (77), so its"
                Write-Host "       port pre-flight was not reached on this host. It said:"
                Write-Host ("       " + ($output -replace '\s+', ' '))
                continue
            }

            $before = $script:SelfTestBad

            Expect "$($row.Name) refuses a held port with a failure, not a skip" $true `
                (($code -ne 0) -and ($code -ne 77))
            Expect "$($row.Name) printed the refusal, holder and all" $true `
                (($output -replace '\s+', ' ').Contains($wantRefusal))

            if ($script:SelfTestBad -ne $before) {
                Write-Host "  --- $($row.Name) exited ${code} saying ---"
                Write-Host $output
                Write-Host "  --- it was expected to say ---"
                Write-Host "  $wantRefusal"
            }
        }
    } finally {
        Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue
    }
}

Export-ModuleMember -Function New-E2EPort, Get-E2EPortHolder, Resolve-E2EPortHolder, Get-E2EPortRefusal, Measure-E2EFormatPrecedence,
                              Resolve-E2EImagePath, Assert-E2EPortAvailable,
                              Get-E2EFixturePort, Wait-E2EPortAnswers, Read-E2ELiveText,
                              Invoke-E2EPortSelfTest
