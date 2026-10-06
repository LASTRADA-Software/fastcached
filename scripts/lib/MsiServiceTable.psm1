# SPDX-License-Identifier: Apache-2.0
#
# What the Windows packaging job asserts about the MSI's service table, and the
# msiexec, package and log helpers it asserts through.
#
# ## Why this is a module rather than copies in each step
#
# `Package (Windows .msi)` drives one package through a chain of transactions --
# upgrades from 0.3.0 and from the latest release, upgrades between two builds of
# this one, a fresh install, a feature change, a repair and an uninstall -- and asserts the
# service table after every one. Its steps share no PowerShell scope, so each
# helper would otherwise be pasted once per step, and a copy that drifts is an
# assertion that no longer says what its neighbours say.
#
# ## The subject and the expectation
#
# The subject is `packaging/windows/service-actions.xml`, which decides from the
# features a transaction leaves installed how each service is registered and
# whether it is started. `$script:ServiceTable` below restates that table as the
# EXPECTATION, one row per state a transaction may leave, so an assertion site
# names a row rather than restating start modes. Only the Windows packaging job
# can compare the two, because only it runs the package against a real service
# control manager.
#
# ## What the self-test can and cannot reach
#
# `Invoke-MsiServiceTableSelfTest` drives the service verdict over synthetic
# observations in both directions, the waiting assertion against a service every
# Windows host runs and a name no host has, the package reader over an MSI
# database it creates, the registry walker over a scratch key in HKCU, and the
# firewall snapshot over a group no host has. It installs nothing. Its blind
# spots, each with the direction it fails in:
#
#   * `Get-InstalledProductCodes` is exercised only for an UpgradeCode nothing is
#     installed under, because a non-empty answer needs an installed product.
#     Fails OPEN.
#   * The table's rows are checked for VOCABULARY only -- a known start mode and a
#     known state -- never against the fragment. A row that is wrong the same way
#     the fragment is wrong (both saying fastcached stays auto beside the node,
#     say) passes here and passes the packaging job, because there the one agrees
#     with the other. Only a reader of both catches that. Fails OPEN.
#   * `Assert-NodeStatePrivate` reads access lists a real install produced; only
#     its pure verdicts run here (stated at the function). Fails CLOSED: it throws.
#   * `Get-InstallationSnapshot` is assembled whole only by the packaging job,
#     which alone has the services, the HKLM keys and the products it reads. Its
#     parts run here -- the walker, the settled state through its observation seam,
#     the comparison over synthetic records -- never the assembly. A field read
#     wrongly reads wrongly on BOTH sides of a comparison, so that fails OPEN; the
#     job's discrimination leg, which requires a successful upgrade to change the
#     fields it names, is what shows those readers live.
#   * `Get-FirewallGroupSnapshot` renders a POPULATED group only on the packaging
#     job; here it reads an absent group live and its error decision through its
#     seam. A line rendered wrongly is wrong on both sides too: fails OPEN.
#   * `Get-SettledServiceState`'s real sleep runs only live; here it is
#     `-StableSeconds 0` through the observation seam.
#   * The transaction judgement (`Invoke-TransactionJudgement`, run by
#     `Invoke-Msiexec` and `Assert-ServiceTable`) judges records the packaging job
#     alone acquires over a real transaction. Here the verdict runs over synthetic
#     records, the watch over a real process through a scripted observer, the
#     event parse over every event shape it reads, and the event reader over the
#     real System log in both directions. On a host that writes no 7036 (this
#     repository's Windows 11 26200 development host writes none), a start that
#     lives and dies between two observations WITHOUT terminating unexpectedly is
#     seen by no witness, and the pass line says "7036 ABSENT". Fails OPEN there.
#
# And one limit of the assertion itself, not of its self-test: the stability
# window is about five seconds after a row matches. A service that crashes
# within it is caught; one that runs longer before dying -- a node that fails
# its first heartbeat a minute in, say -- still passes.

Set-StrictMode -Version Latest

# A module reads preference variables from its OWN scope chain, not its caller's,
# so a step's `$ErrorActionPreference = 'Stop'` does not reach these functions.
# Set here, every failure below ends the transaction's step rather than printing
# and carrying on -- which is what a COM type mismatch did, measured, before this
# line: the error was printed and the next statement ran.
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# The expectation
# ---------------------------------------------------------------------------

# One row per state a transaction may leave. A value is the start mode and the
# state the service must reach, as Win32_Service spells them; $null means the
# service must not be registered at all.
#
#   Release030WithNode  -- 0.3.0 installed with both node properties: fastcached
#                          auto and started, the node registered auto and never
#                          started (that installer starts only fastcached). The
#                          starting point of the upgrade, not a row of the
#                          current table.
#   NodeSelected        -- the node feature is installed: node auto and running,
#                          fastcached manual and stopped (both would answer on 6674).
#   NodeAlone           -- the node without fastcached: node auto and running, and
#                          no fastcached registration left behind.
#   DaemonAlone         -- fastcached without the node: auto and running, and no
#                          node registration left behind.
#   DaemonAloneUnstarted -- the same, installed with FASTCACHED_START_SERVICE=0:
#                          registered auto and left stopped.
#   NothingInstalled    -- after an uninstall.
$script:ServiceTable = [ordered]@{
    Release030WithNode   = [ordered]@{ FastCached = @('Auto', 'Running'); FastCacheCompileNode = @('Auto', 'Stopped') }
    NodeSelected         = [ordered]@{ FastCacheCompileNode = @('Auto', 'Running'); FastCached = @('Manual', 'Stopped') }
    NodeAlone            = [ordered]@{ FastCacheCompileNode = @('Auto', 'Running'); FastCached = $null }
    DaemonAlone          = [ordered]@{ FastCacheCompileNode = $null; FastCached = @('Auto', 'Running') }
    DaemonAloneUnstarted = [ordered]@{ FastCacheCompileNode = $null; FastCached = @('Auto', 'Stopped') }
    NothingInstalled     = [ordered]@{ FastCacheCompileNode = $null; FastCached = $null }
}

# The vocabulary a row may use, so a misspelt mode fails the self-test rather than
# every assertion that names the row.
$script:StartModes = @('Auto', 'Manual', 'Disabled')
$script:States = @('Running', 'Stopped')

# The services this package owns, by service name, each with the display name its
# registration carries (`ServiceSpec::displayName`). A service control manager event
# names a service by its display name in its text and by its service name in its
# binary data; either is recognised, so a registration another build wrote under
# another display name is still matched by its service name.
$script:ServiceDisplayNames = [ordered]@{ FastCached = 'fastcached'; FastCacheCompileNode = 'fastcache-compile-node' }

# The service control manager's events for a service that ended without being told
# to: 7031 while the SCM still takes a recovery action for it, 7034 when it takes
# none. The registrations' recovery steps are FINITE (`ServiceRestartAttempts`
# restarts, then no action), so one service logs both: PR 1634's CI showed the node
# logging 7034 at its eighth to eleventh termination.
$script:UnexpectedTerminationEvents = @(7031, 7034)

# What a transaction is judged over: the terminations, and "entered the running
# state" (7036) where the host writes it.
$script:TransactionEvents = @(7031, 7034, 7036)

# The last transaction Invoke-Msiexec ran: what each service ran as when it began,
# when it began, what it was expected to leave, and every process the watch saw a
# service run under while it ran. Invoke-Msiexec judges it as it ends, and
# Assert-ServiceTable once more over the window its stability interval adds.
$script:LastMsiTransaction = $null

# ---------------------------------------------------------------------------
# Services
# ---------------------------------------------------------------------------

# What the service control manager reports for one service right now.
#
# Win32_Service rather than Get-Service: it carries the start mode and the
# process id, which the other does not, and reading all three from one query
# means the parts of an observation cannot come from different moments.
#
# @param Name The service name.
# @return $null when no such service is registered, else an object carrying
#         StartMode, State and ProcessId (0 when no process runs it).
function Get-ServiceObservation([string] $Name) {
    $svc = Get-CimInstance Win32_Service -Filter "Name='$Name'"
    if (-not $svc) { return $null }
    return [pscustomobject]@{ StartMode = $svc.StartMode; State = $svc.State; ProcessId = [int]$svc.ProcessId }
}

# The decision, apart from the acquisition: whether an observation satisfies an
# expectation, and if not, why.
#
# @param Name The service name, for the message.
# @param Want $null for "not registered", else @(start mode, state).
# @param Seen What Get-ServiceObservation returned.
# @return $null when satisfied, else a sentence naming the difference.
function Get-ServiceVerdict([string] $Name, $Want, $Seen) {
    if ($null -eq $Want) {
        if ($null -eq $Seen) { return $null }
        return "$Name is still registered ($($Seen.StartMode), $($Seen.State)); expected no registration"
    }
    $mode, $state = $Want
    if ($null -eq $Seen) { return "$Name is not registered; expected $mode and $state" }
    if ($Seen.StartMode -ne $mode) { return "$Name is registered '$($Seen.StartMode)', expected '$mode'" }
    if ($Seen.State -ne $state) { return "$Name is $($Seen.State), expected $state" }
    return $null
}

# Whether a service that matched its expectation is still the SAME service a
# moment later: same start mode, same state, same process.
#
# One observation of Running cannot tell a service that runs from one that
# crashes and is restarted by its recovery policy, which the node has: each start
# reads Running until the process dies. A second observation that finds another
# process, or another state, is the difference.
#
# @param Name The service name, for the message.
# @param First The observation that matched.
# @param Second The observation taken after the interval.
# @param Seconds The interval, for the message.
# @return $null when both describe one running (or one stopped) service, else a
#         sentence naming the change.
function Get-ServiceStabilityVerdict([string] $Name, $First, $Second, [double] $Seconds) {
    if ($null -eq $Second) { return "$Name was deregistered within $Seconds s of matching" }
    if ($Second.StartMode -ne $First.StartMode) {
        return "$Name changed start mode from $($First.StartMode) to $($Second.StartMode) within $Seconds s"
    }
    if ($Second.State -ne $First.State) {
        return "$Name went from $($First.State) to $($Second.State) within $Seconds s"
    }
    if ($Second.State -eq 'Running' -and ($First.ProcessId -le 0 -or $Second.ProcessId -le 0)) {
        return "$Name is Running with no process id (process $($First.ProcessId), then $($Second.ProcessId)), so whether it restarted cannot be told"
    }
    if ($Second.ProcessId -ne $First.ProcessId) {
        return "$Name restarted within $Seconds s (process $($First.ProcessId) became $($Second.ProcessId)): a service its recovery policy keeps restarting is crash-looping, not running"
    }
    return $null
}

# Waits, bounded, until every service matches its expectation, then requires each
# registered one to stay as it was for a further interval, then reports each.
#
# A start is asynchronous, and so is a deletion: a registration marked for
# deletion stays visible while anything holds a handle to it. So every row is
# waited for rather than read once. The bound is measured on a monotonic clock,
# never counted in iterations. The interval is ONE sleep for the whole row, after
# every service has matched, so it costs a transaction seconds, not a service.
#
# @param Expect Service name to expectation, as a Get-ServiceVerdict Want.
# @param TimeoutSeconds How long each service may take.
# @param StableSeconds How long a matched registration must stay unchanged.
# @param Observe Takes a service name and returns what Get-ServiceObservation
#                would; the seam through which the self-test scripts a restart.
function Assert-ServiceState {
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Expect,
        [int] $TimeoutSeconds = 30,
        [int] $StableSeconds = 5,
        [scriptblock] $Observe = { param($name) Get-ServiceObservation $name }
    )
    $matched = [ordered]@{}
    foreach ($name in $Expect.Keys) {
        $want = $Expect[$name]
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            $seen = & $Observe $name
            $verdict = Get-ServiceVerdict $name $want $seen
            if ($null -eq $verdict) { break }
            if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                throw "$verdict (waited $([math]::Round($clock.Elapsed.TotalSeconds, 1)) s)"
            }
            Start-Sleep -Milliseconds 500
        }
        if ($null -ne $want) { $matched[$name] = $seen }
    }
    if ($matched.Count -gt 0) {
        Start-Sleep -Seconds $StableSeconds
        foreach ($name in $matched.Keys) {
            $verdict = Get-ServiceStabilityVerdict $name $matched[$name] (& $Observe $name) $StableSeconds
            if ($null -ne $verdict) { throw $verdict }
        }
    }
    foreach ($name in $Expect.Keys) {
        $want = $Expect[$name]
        $described = if ($null -eq $want) { 'not registered' } else {
            "$($want -join ' and '), process $($matched[$name].ProcessId), unchanged for $StableSeconds s"
        }
        Write-Host "  ${name}: $described"
    }
}

# Asserts one row of the service table.
#
# @param Row A key of $script:ServiceTable.
# @param Log The verbose log of the transaction that should have left this row,
#            shown when it did not: on a CI runner the log is the only witness.
# @param TimeoutSeconds How long each service may take.
# @param StableSeconds How long a matched registration must stay unchanged.
# @param Observe As Assert-ServiceState's; the self-test's seam.
# @param ReadEvents As Invoke-TransactionJudgement's; the self-test's seam.
function Assert-ServiceTable {
    param(
        [Parameter(Mandatory)] [string] $Row,
        [string] $Log = '',
        [int] $TimeoutSeconds = 30,
        [int] $StableSeconds = 5,
        [scriptblock] $Observe = { param($name) Get-ServiceObservation $name },
        [scriptblock] $ReadEvents = { param($since) Get-ServiceControlEvents -SinceUtc $since }
    )
    if (-not $script:ServiceTable.Contains($Row)) {
        throw "no service table row '$Row'; the rows are: $($script:ServiceTable.Keys -join ', ')"
    }
    Write-Host "service table: $Row"
    try {
        Assert-ServiceState -Expect $script:ServiceTable[$Row] -TimeoutSeconds $TimeoutSeconds -StableSeconds $StableSeconds -Observe $Observe
        # The transaction that left this row, judged again now that the window reaches past the
        # stability interval: a service restarted by its recovery policy after msiexec returned
        # terminates inside it. Once per transaction.
        $transaction = $script:LastMsiTransaction
        if ($null -ne $transaction -and -not $transaction.Extended) {
            $transaction.Extended = $true
            Write-Host (Invoke-TransactionJudgement -Transaction $transaction -ReadEvents $ReadEvents)
        }
    } catch {
        if ($Log) { Show-MsiLog $Log }
        foreach ($name in $script:ServiceTable[$Row].Keys) {
            if ($null -ne $script:ServiceTable[$Row][$name]) { Show-ServiceDiagnosis $name }
        }
        throw
    }
}

# A registered command line as the arguments a process receives: a token is a run of
# unquoted characters and quoted sections, with the quotes dropped -- not `"..."|\S+`,
# whose alternation splits `--cache-dir="C:\dir with space"` into three.
#
# @param PathName The service's registered command line.
# @return The program, then each argument. A single token comes back as a scalar, as
#         PowerShell returns any one-element array, so a caller wraps the call in `@( )`.
function Split-RegisteredCommandLine([string] $PathName) {
    return @([regex]::Matches($PathName, '(?:[^\s"]|"[^"]*")+') | ForEach-Object { $_.Value -replace '"', '' })
}

# Everything a CI runner can say about why a service is not in the state its row
# expects -- the run's only witness, since nothing on the runner survives the job.
#
# The registration and the exit code the SCM recorded (the node exits 78 on a startup
# refusal); the service's own Application events, where `--daemon` logs; the SCM's System
# events naming it; and the registered command line run in the FOREGROUND for 15 s without
# `--daemon`, so a refusal arrives as its text. That last run is this account, not the
# service's, so a cause that is the account's own -- a state file it cannot read -- shows
# only in the events. Never throws: it runs on the way to a throw that matters more.
#
# @param Name The service name.
function Show-ServiceDiagnosis([string] $Name) {
    Write-Host "===== $Name (diagnosis) ====="
    try {
        $svc = Get-CimInstance Win32_Service -Filter "Name='$Name'"
        if (-not $svc) { Write-Host 'not registered'; return }
        Write-Host "registered: $($svc.PathName)"
        Write-Host "account $($svc.StartName); $($svc.StartMode), $($svc.State); exit code $($svc.ExitCode), service-specific $($svc.ServiceSpecificExitCode)"

        Write-Host "--- Application events from $Name (last 10 min) ---"
        Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = $Name; StartTime = (Get-Date).AddMinutes(-10) } `
            -ErrorAction SilentlyContinue | Select-Object -First 20 | ForEach-Object { "$($_.TimeCreated) [$($_.LevelDisplayName)] $($_.Message)" }
        Write-Host "--- System events naming $Name (last 10 min) ---"
        Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = (Get-Date).AddMinutes(-10) } -ErrorAction SilentlyContinue |
            Where-Object { $_.Message -match [regex]::Escape($Name) } | Select-Object -First 10 |
            ForEach-Object { "$($_.TimeCreated) [$($_.Id)] $($_.Message)" }

        if ($svc.State -eq 'Running') { return }
        $argv = @(Split-RegisteredCommandLine $svc.PathName)
        $out = Join-Path ([IO.Path]::GetTempPath()) "$Name-foreground.log"
        $proc = Start-Process -FilePath $argv[0] -ArgumentList @($argv | Select-Object -Skip 1 | Where-Object { $_ -ne '--daemon' }) `
            -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError "$out.err"
        if ($proc.WaitForExit(15000)) { Write-Host "--- the registered command line in the foreground exited $($proc.ExitCode) ---" }
        else { $proc.Kill(); Write-Host '--- the registered command line in the foreground was still running after 15 s, so it started ---' }
        Get-Content $out, "$out.err" -ErrorAction SilentlyContinue | Select-Object -Last 40
    } catch {
        Write-Host "the diagnosis itself failed: $_"
    }
}

# ---------------------------------------------------------------------------
# Packages
# ---------------------------------------------------------------------------

# Reads the first column of the first row of one query against a package's database, read-only.
#
# The ONE place the module reads a package through COM: every COM object is released before
# returning, so the package is not held open when msiexec or cpack next needs it.
#
# @param Path The .msi file.
# @param Query MSI SQL (backtick-quoted identifiers).
# @return The value as text, or $null when the query matches no row; each caller owns what that means.
function Get-MsiScalar([string] $Path, [string] $Query) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    try {
        $db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @([string]$Path, 0))
        try {
            $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @([string]$Query))
            try {
                $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
                $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
                if (-not $record) { return $null }
                try {
                    return $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, @(1))
                } finally {
                    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($record)
                }
            } finally {
                $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
                [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
            }
        } finally {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($db)
        }
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
}

# Reads one row of a package's Property table.
#
# @param Path The .msi file.
# @param Name The property.
# @return Its value; a package without it is refused by name.
function Get-MsiProperty([string] $Path, [string] $Name) {
    $value = Get-MsiScalar $Path "SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$Name'"
    if ($null -eq $value) { throw "$Path carries no $Name property" }
    return $value
}

# Reads the Type of one row of a package's CustomAction table (#1629): the bit field that says
# whether the action is deferred, committed, run in the system context, and so on.
#
# @param Path The .msi file.
# @param Action The custom action's name.
# @return Its Type as an integer; an action the package does not carry is refused by name.
function Get-MsiCustomActionType([string] $Path, [string] $Action) {
    $value = Get-MsiScalar $Path "SELECT ``Type`` FROM ``CustomAction`` WHERE ``Action`` = '$Action'"
    if ($null -eq $value) { throw "$Path carries no $Action custom action" }
    return [int]$value
}

# The ProductCodes Windows Installer holds as installed under one UpgradeCode.
#
# This is what tells an upgrade from a side-by-side install: after a major
# upgrade exactly the new product is left, and two entries mean the old product
# was never detected.
#
# @param UpgradeCode The product family.
# @return The installed ProductCodes, always an array.
function Get-InstalledProductCodes([string] $UpgradeCode) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    try {
        $list = $installer.GetType().InvokeMember('RelatedProducts', 'GetProperty', $null, $installer, @($UpgradeCode))
        $count = $list.GetType().InvokeMember('Count', 'GetProperty', $null, $list, $null)
        # 0..-1 counts DOWN in PowerShell, so an empty list must not reach the range.
        $codes = @(if ($count -gt 0) {
                foreach ($index in 0..($count - 1)) {
                    $list.GetType().InvokeMember('Item', 'GetProperty', $null, $list, @([int]$index))
                }
            })
        return , $codes
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
}

# Whether this build can be installed over a release as an upgrade. A build's
# version comes from the nearest tag, so a branch that does not contain the
# latest release's tag builds an OLDER version, and installing it over that
# release is a downgrade msiexec refuses with 1603 -- which reads like a defect
# in the service table rather than a branch behind master. An equal version is
# an upgrade here: CPack's MajorUpgrade allows the same version.
#
# @param ThisVersion This build's ProductVersion.
# @param LatestVersion The release's ProductVersion.
# @param LatestTag The release's tag, for the message.
# @return $null when this build is not older, else the refusal naming the cause.
function Get-UpgradeVersionVerdict([string] $ThisVersion, [string] $LatestVersion, [string] $LatestTag) {
    $this = $null
    $latest = $null
    if (-not [version]::TryParse($ThisVersion, [ref]$this)) { return "this build's ProductVersion '$ThisVersion' is not a version" }
    if (-not [version]::TryParse($LatestVersion, [ref]$latest)) { return "$LatestTag's ProductVersion '$LatestVersion' is not a version" }
    if ($this -lt $latest) {
        return "this build is $ThisVersion, older than the latest release $LatestTag ($LatestVersion); rebase onto a tree that contains the tag"
    }
    return $null
}

# Asserts that exactly one product of a family is installed, and which.
#
# @param UpgradeCode The product family.
# @param ProductCode The one product that must be installed, or '' for none.
function Assert-InstalledProduct([string] $UpgradeCode, [string] $ProductCode) {
    $installed = Get-InstalledProductCodes $UpgradeCode
    $expected = @($ProductCode | Where-Object { $_ })
    if (Compare-Object -ReferenceObject $expected -DifferenceObject $installed) {
        throw "installed under $UpgradeCode`: [$($installed -join ', ')]; expected [$($expected -join ', ')]"
    }
    Write-Host "installed under ${UpgradeCode}: [$($installed -join ', ')]"
}

# Removes what an installed product leaves behind on purpose -- the seeded
# configuration and the node's state directory with its identity key -- so the
# next step starts from a machine that never had this product, and refuses by
# name when a directory survives.
function Remove-FastCacheMachineState {
    foreach ($dir in "$env:ProgramData\fastcached", "$env:ProgramData\fastcache-node") {
        Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        if (Test-Path $dir) { throw "$dir survived its removal, so the next install would read the previous one's files" }
    }
}

# The firewall rules the MSI's node registration opens: what the SERVICE opens at its next
# start, which since the zero-config defaults is consensus and discovery beside the node port --
# with the discovery reply port the PACKAGE pins (FASTCACHE_DISCOVERY_REPLY_PORT, default 6682),
# so no rule admits any local port. DocumentedCommandLines_test's [msi] case derives the same
# reply rule from the fragment's own command line.
$script:NodeFirewallRuleNames = @(
    'FastCacheCompileNode node tcp/6674',
    'FastCacheCompileNode raft tcp/6680',
    'FastCacheCompileNode discovery-beacon udp/6681',
    'FastCacheCompileNode discovery-reply udp/6682')

# Does the node's firewall group hold exactly @p Expected? A pure verdict so the self-test can
# drive it without a real firewall.
# @param Seen The display names the group holds.
# @param Expected The names it must hold.
# @return $null when the sets agree, else what is missing and what is extra.
function Get-NodeFirewallVerdict([string[]] $Seen, [string[]] $Expected) {
    $missing = @($Expected | Where-Object { $Seen -notcontains $_ })
    $extra = @($Seen | Where-Object { $Expected -notcontains $_ })
    if ($missing.Count -eq 0 -and $extra.Count -eq 0) { return $null }
    return "the node's firewall group lacks [$($missing -join ', ')] and carries [$($extra -join ', ')] beyond what its service opens"
}

# The node's firewall rules after an install, on the REAL Windows Firewall: the group holds
# exactly what the service will open. The upgrade over an exposed 0.3.0 state directory is the
# case this exists for -- the formation record is held until the install secures the directory,
# and rules derived before that opened the node port alone. Reads a live firewall, so the
# self-test reaches only the verdict above; fails CLOSED.
function Assert-NodeFirewall {
    $group = 'fastcached: FastCacheCompileNode'
    $rules = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)
    $rules | Format-Table DisplayName, Direction, Action, Profile | Out-String | Write-Host
    if ($verdict = Get-NodeFirewallVerdict @($rules | ForEach-Object { $_.DisplayName }) $script:NodeFirewallRuleNames) {
        throw $verdict
    }
}

# Is the firewall group @p Group empty? A pure verdict so the self-test can drive it without a real
# firewall. W-11: an upgrade that deselects a feature deletes its service with sc.exe, which removes
# no rule, so the fragment removes the group and this checks it went.
# @param Seen The display names the group holds.
# @param Group The group, for the message.
# @return $null when it holds nothing, else what it still holds.
function Get-FirewallGroupEmptyVerdict([string[]] $Seen, [string] $Group) {
    $held = @($Seen | Where-Object { $_ })
    if ($held.Count -eq 0) { return $null }
    return "the firewall group '$Group' still holds [$($held -join ', ')] after its service was removed"
}

# The firewall group @p Group on the REAL Windows Firewall holds no rule. Reads a live firewall, so the
# self-test reaches only the verdict above; fails CLOSED. Assert the group was POPULATED before the
# transaction that must empty it (Assert-NodeFirewall), or an empty group here proves nothing.
function Assert-FirewallGroupEmpty([string] $Group) {
    $rules = @(Get-NetFirewallRule -Group $Group -ErrorAction SilentlyContinue)
    $rules | Format-Table DisplayName, Direction, Action, Profile | Out-String | Write-Host
    if ($verdict = Get-FirewallGroupEmptyVerdict @($rules | ForEach-Object { $_.DisplayName }) $Group) { throw $verdict }
}

# One firewall rule as one snapshot line: every property a registration's firewall step sets --
# name, enabled, direction, action, PROFILE, protocol, local port, remote address and the PROGRAM it
# admits. A pure formatter, so the self-test can show a property the line omits would let a rollback
# that changed it pass (round 11 review, M6: profile and program were omitted).
# @param Rule The rule: DisplayName, Enabled, Direction, Action, Profile.
# @param Port Its port filter: Protocol, LocalPort.
# @param Address Its address filter: RemoteAddress.
# @param Application Its application filter: Program.
# @return The line.
function Format-FirewallRuleLine($Rule, $Port, $Address, $Application) {
    return "$($Rule.DisplayName) | enabled=$($Rule.Enabled) $($Rule.Direction) $($Rule.Action) profile=$($Rule.Profile) | $($Port.Protocol)/$(@($Port.LocalPort) -join ',') | remote=$(@($Address.RemoteAddress) -join ',') | program=$($Application.Program)"
}

# The error Get-NetFirewallRule raises for a group that holds no rule, by its id, measured on
# PowerShell 7.6 (2026-10-06). The ONLY error a snapshot reads as "empty": any other -- access denied,
# the firewall service down, a CIM failure -- is a read that FAILED, and an empty answer for it
# would compare as "unchanged" on both sides of a transaction.
$script:FirewallGroupNotFoundId = 'CmdletizationQuery_NotFound_RuleGroup,Get-NetFirewallRule'

# The firewall group @p Group on the REAL Windows Firewall, one Format-FirewallRuleLine per rule,
# sorted, so two snapshots compare as text. What Assert-FirewallGroupUnchanged reads.
# @param Group The group, `fastcached: <service>`.
# @param Read Takes a group and returns its rules, as Get-NetFirewallRule -ErrorAction Stop does; a
#        seam so the self-test can drive the error decision without a firewall.
# @return The lines; none for an empty or absent group. Throws on a read that failed for any other
#         reason than the group holding no rule.
function Get-FirewallGroupSnapshot {
    param(
        [Parameter(Mandatory)] [string] $Group,
        [scriptblock] $Read = { param($group) Get-NetFirewallRule -Group $group -ErrorAction Stop }
    )
    $rules = @(try { & $Read $Group } catch {
            if ($_.FullyQualifiedErrorId -cne $script:FirewallGroupNotFoundId) { throw }
        })
    return @($rules | ForEach-Object {
            Format-FirewallRuleLine $_ ($_ | Get-NetFirewallPortFilter) ($_ | Get-NetFirewallAddressFilter) `
                ($_ | Get-NetFirewallApplicationFilter)
        } | Sort-Object)
}

# The elements of @p From that @p Without does not match ONE FOR ONE, case-sensitively: a MULTISET
# difference, so a line held twice where it was held once is a difference. A set test
# (-cnotcontains) reads @('r') and @('r', 'r') as equal, and a firewall rule a rollback duplicated
# renders as exactly that line twice (Format-FirewallRuleLine carries no rule Name).
# @param From The lines to look for.
# @param Without The lines to match them against, each usable once.
# @return The unmatched lines of @p From, in its order; nothing when all matched.
function Get-MultisetDifference([string[]] $From, [string[]] $Without) {
    $remaining = [System.Collections.Generic.List[string]]::new()
    foreach ($line in @($Without)) { $remaining.Add($line) }
    foreach ($line in @($From)) {
        # List[string].IndexOf compares ordinally, so case-sensitively, as -cnotcontains did.
        $at = $remaining.IndexOf($line)
        if ($at -ge 0) { $remaining.RemoveAt($at) } else { $line }
    }
}

# Does the group hold, after a transaction, exactly what it held before? A pure verdict so the
# self-test can drive it without a firewall. An EMPTY before is refused rather than compared: two
# empty snapshots agree perfectly, and a group that was never populated says nothing about a rollback.
# @param Before The snapshot taken before the transaction.
# @param After The snapshot taken after it.
# @param Group The group, for the message.
# @return $null when they agree, else what the rollback lost and what it left behind.
function Get-FirewallGroupUnchangedVerdict([string[]] $Before, [string[]] $After, [string] $Group) {
    $before = @($Before | Where-Object { $_ })
    $after = @($After | Where-Object { $_ })
    if ($before.Count -eq 0) { return "the firewall group '$Group' held no rule BEFORE the transaction, so an unchanged group proves nothing" }
    $lost = @(Get-MultisetDifference $before $after)
    $left = @(Get-MultisetDifference $after $before)
    if ($lost.Count -eq 0 -and $left.Count -eq 0) { return $null }
    return "the firewall group '$Group' changed: it lost [$($lost -join '; ')] and holds [$($left -join '; ')] it did not"
}

# The live firewall group @p Group holds exactly @p Before. Reads a live firewall, so the self-test
# reaches only the verdict above; fails CLOSED.
function Assert-FirewallGroupUnchanged([string] $Group, [string[]] $Before) {
    $after = Get-FirewallGroupSnapshot $Group
    $after | ForEach-Object { Write-Host "  $_" }
    if ($verdict = Get-FirewallGroupUnchangedVerdict $Before $after $Group) { throw $verdict }
}

# Does every rule of the node's firewall group admit exactly @p Scope as its remote address? A pure
# verdict so the self-test can drive it without a real firewall. Windows reports a prefix in mask
# form (10.0.0.0/8 reads back as 10.0.0.0/255.0.0.0), so both spellings of the scope are accepted.
# @param Seen Each rule's display name and its remote addresses, as @{ Name; Remote }.
# @param Scope The one address with a /prefix the install was given.
# @return $null when every rule admits exactly the scope, else which rules admit what.
function Get-NodeFirewallScopeVerdict([object[]] $Seen, [string] $Scope) {
    $address, $prefix = $Scope -split '/'
    $accepted = @($Scope)
    if ($prefix) {
        $bits = ([uint64]4294967295 -shl (32 - [int]$prefix)) -band [uint64]4294967295
        $mask = (3, 2, 1, 0 | ForEach-Object { ($bits -shr (8 * $_)) -band 0xFF }) -join '.'
        $accepted += "$address/$mask"
    }
    if ($Seen.Count -eq 0) { return "the node's firewall group holds no rule to scope" }
    $wrong = @($Seen | Where-Object { @($_.Remote).Count -ne 1 -or $accepted -notcontains @($_.Remote)[0] })
    if ($wrong.Count -eq 0) { return $null }
    return "rules not scoped to ${Scope}: " + (($wrong | ForEach-Object { "$($_.Name) admits [$(@($_.Remote) -join ', ')]" }) -join '; ')
}

# The node's firewall rules on the REAL Windows Firewall admit exactly @p Scope. A repair that
# states no scope must keep the one an earlier transaction stated (the remembered property), so
# a rule reading Any here is the fail-open this exists to catch. Reads a live firewall, so the
# self-test reaches only the verdict above; fails CLOSED.
function Assert-NodeFirewallScope([string] $Scope) {
    $group = 'fastcached: FastCacheCompileNode'
    $seen = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue | ForEach-Object {
            @{ Name = $_.DisplayName; Remote = @(($_ | Get-NetFirewallAddressFilter).RemoteAddress) }
        })
    $seen | ForEach-Object { Write-Host "$($_.Name): $($_.Remote -join ', ')" }
    if ($verdict = Get-NodeFirewallScopeVerdict $seen $Scope) { throw $verdict }
}

# Does the node service's command line carry @p Argument as one whole token? A pure verdict so the
# self-test can drive it without a service. Whole tokens, because a prefix match would pass a
# registration carrying a longer value, and a quoted program path may hold spaces.
# @param ImagePath The registration's command line, as the service control manager reports it.
# @param Argument The token it must carry, e.g. a fleet-seed flag and its normalized value.
# @return $null when it is there, else the command line it is missing from.
function Get-NodeRegistrationArgumentVerdict([string] $ImagePath, [string] $Argument) {
    $tokens = @([regex]::Matches($ImagePath, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
    if ($tokens -ccontains $Argument) { return $null }
    return "the node's registration does not carry $Argument as an argument: $ImagePath"
}

# The node's REAL registration carries @p Argument. A repair that states no fleet seed must keep the
# one an earlier transaction stated (the remembered property). Reads a live service, so the
# self-test reaches only the verdict above; fails CLOSED.
function Assert-NodeRegistrationArgument([string] $Argument) {
    $svc = Get-CimInstance Win32_Service -Filter "Name='FastCacheCompileNode'"
    if (-not $svc) { throw "no FastCacheCompileNode service is registered, so it carries no $Argument" }
    Write-Host "FastCacheCompileNode: $($svc.PathName)"
    if ($verdict = Get-NodeRegistrationArgumentVerdict $svc.PathName $Argument) { throw $verdict }
}

# Does the node service's command line carry NO token for the flag @p Prefix -- the flag itself, or the
# flag followed by `=`? A pure verdict so the self-test can drive it without a service. Two flags an
# earlier package registered and this node must not keep: `--scheduler`, which it refuses at every
# start (ci-fix2), and `--advertise`, from a remembered property this package no longer has (W-10).
# A longer flag that merely shares the prefix is not the flag, and a quoted program path is one token.
# @param ImagePath The registration's command line, as the service control manager reports it.
# @param Prefix The flag, e.g. `--scheduler`.
# @return $null when no token carries it, else the tokens and the command line.
function Get-NodeRegistrationLacksVerdict([string] $ImagePath, [string] $Prefix) {
    $tokens = @([regex]::Matches($ImagePath, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
    $hit = @($tokens | Where-Object { $_ -ceq $Prefix -or $_.StartsWith("$Prefix=", [StringComparison]::Ordinal) })
    if ($hit.Count -eq 0) { return $null }
    return "the node's registration still carries $($hit -join ' '), so it is the one an earlier package made: $ImagePath"
}

# The node's REAL registration carries no token for @p Prefix. Reads a live service, so the self-test
# reaches only the verdict above; fails CLOSED on no registration.
function Assert-NodeRegistrationLacks([string] $Prefix) {
    $svc = Get-CimInstance Win32_Service -Filter "Name='FastCacheCompileNode'"
    if (-not $svc) { throw "no FastCacheCompileNode service is registered, so nothing can be said about $Prefix" }
    Write-Host "FastCacheCompileNode: $($svc.PathName)"
    if ($verdict = Get-NodeRegistrationLacksVerdict $svc.PathName $Prefix) { throw $verdict }
}

# Is @p OwnerSid the Administrators SID? The state directory's owner must be, so whoever
# created it first keeps no WRITE_DAC. A pure verdict so the self-test can drive it without a
# real directory, the way the service verdicts are.
# @param OwnerSid The directory's owner, as a SID string.
# @return $null when it is Administrators, else why not.
function Get-DirectoryOwnerVerdict([string] $OwnerSid) {
    if ($OwnerSid -ne 'S-1-5-32-544') { return "owner is $OwnerSid, expected Administrators (S-1-5-32-544)" }
    return $null
}

# The node's state directory, and the identity key in it, answer to nobody but
# SYSTEM, Administrators and the service. A function rather than a copy per step,
# for the module's own reason: two steps assert this -- the feature change and the
# upgrade from an exposed 0.3.0 directory -- and a copy that drifts stops saying
# what its neighbour says. It reads real access lists a real install produced, so
# the self-test cannot reach it; that is one of the blind spots named at the top of
# this module, and it fails CLOSED -- it throws.
#
# Whether the node's service reaches its state directory and its key the way the install means it
# to: it may ADD to the directory -- it mints the key there -- and may NOT rewrite the directory's
# list (M1), since this process compiles input that arrived over the network and the directory's
# list is what keeps every other account from planting a file it trusts; and it reads its key.
#
# The key is created with a list of its own (`OwnerOnlySecretFileDacl`: SYSTEM, Administrators and
# OWNER RIGHTS), so the service reaches it as its OWNER rather than through an entry naming it;
# either route counts. Only entries that apply to the object ITSELF count: an inherit-only entry
# grants its rights to what is created inside, not to the directory.
#
# A function over two paths and a SID, so the self-test drives it over a real access list under
# this module's strict mode -- where `.Count` on a pipeline's lone result is an error, which is how
# the inline version of this threw on exactly the list the install produces (round 6, C1).
#
# @param State The state directory.
# @param Key The identity key inside it.
# @param ServiceSid The service's SID, as a string.
# @return $null when the service reaches both as intended, else why not.
function Get-NodeServiceAccessVerdict([string] $State, [string] $Key, [string] $ServiceSid) {
    $rights = [Security.AccessControl.FileSystemRights]
    $allowsOf = { param($Path, $Sid) @((Get-Acl $Path).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
                  Where-Object { $_.IdentityReference.Value -eq $Sid -and $_.AccessControlType -eq 'Allow' -and
                                 -not ($_.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) }) }
    $rightsOf = { param($Rules) $union = [int64]0; foreach ($rule in $Rules) { $union = $union -bor [int64]$rule.FileSystemRights }; $union }

    $inDirectory = @(& $allowsOf $State $ServiceSid)
    if ($inDirectory.Count -eq 0) { return "nothing in $State's list names the service ($ServiceSid)" }
    if (((& $rightsOf $inDirectory) -band [int64]$rights::CreateFiles) -eq 0) {
        return "the service may not add a file to $State, so it cannot mint its key: $($inDirectory.FileSystemRights)"
    }
    $writeList = [int64]$rights::ChangePermissions -bor [int64]$rights::TakeOwnership
    if (((& $rightsOf $inDirectory) -band $writeList) -ne 0) {
        return "the service may rewrite $State's access list: $($inDirectory.FileSystemRights)"
    }

    $keyOwner = (Get-Acl $Key).GetOwner([Security.Principal.SecurityIdentifier]).Value
    $reach = @(& $allowsOf $Key $ServiceSid)
    if ($keyOwner -eq $ServiceSid) { $reach += @(& $allowsOf $Key 'S-1-3-4') }
    if (((& $rightsOf $reach) -band [int64]$rights::ReadData) -eq 0) {
        return "the service cannot read its own key: owned by $keyOwner, entries $(((Get-Acl $Key).Access | ForEach-Object { "$($_.IdentityReference)=$($_.FileSystemRights)" }) -join ', ')"
    }
    return $null
}

# BOTH directions: nothing broad reads or plants, the list is protected, its owner
# keeps only READ_CONTROL, the service may add to the directory but not rewrite its
# list, and the service still reaches its key (`Get-NodeServiceAccessVerdict`).
function Assert-NodeStatePrivate {
    $state = Join-Path $env:ProgramData 'fastcache-node'
    $key = Join-Path $state 'node-key'
    for ($i = 0; $i -lt 30 -and -not (Test-Path $key); $i++) { Start-Sleep 1 }
    if (-not (Test-Path $key)) {
        throw "the node minted no identity key at $key within 30 s, so the ACL assertions have nothing to read"
    }
    icacls $state; icacls $key

    # FileTrust's BroadPrincipals: Everyone, Authenticated Users, BUILTIN\Users, INTERACTIVE, Guests.
    $broad = @('S-1-1-0', 'S-1-5-11', 'S-1-5-32-545', 'S-1-5-4', 'S-1-5-32-546')
    $lowWord = [int64]4294967295
    $rights = [Security.AccessControl.FileSystemRights]
    # ReadingRights and PlantingRights: the generic bits are numbers because FileSystemRights
    # has no member for them, and an inherited entry often carries them.
    $reading = ([int64]$rights::ReadData) -bor [int64]2147483648 -bor [int64]268435456
    $planting = ([int64]$rights::CreateFiles) -bor [int64]$rights::CreateDirectories -bor [int64]$rights::Delete `
              -bor [int64]$rights::ChangePermissions -bor [int64]$rights::TakeOwnership -bor [int64]1073741824
    foreach ($target in @(@{ Path = $state; Rights = ($reading -bor $planting) }, @{ Path = $key; Rights = $reading })) {
        $rules = @((Get-Acl $target.Path).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
                   Where-Object { $_.AccessControlType -eq 'Allow' })
        if ($rules.Count -eq 0) { throw "read no allow entries from $($target.Path), so the check below would pass on nothing" }
        $mask = ([int64]$target.Rights) -band $lowWord
        $open = @($rules | Where-Object { $broad -contains $_.IdentityReference.Value -and ((([int64]$_.FileSystemRights) -band $lowWord) -band $mask) -ne 0 })
        if ($open.Count -gt 0) {
            $open | ForEach-Object { Write-Host "::error::$($_.IdentityReference) on $($target.Path): $($_.FileSystemRights)" }
            throw "$($target.Path) is open to a broad principal"
        }
    }

    # Protected, so a later change to %ProgramData%'s list cannot flow back in.
    if (-not (Get-Acl $state).AreAccessRulesProtected) { throw "$state still inherits its parent's access list" }

    # The OWNER is Administrators, so whoever created the directory first keeps no WRITE_DAC.
    $ownerSid = (Get-Acl $state).GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($verdict = Get-DirectoryOwnerVerdict $ownerSid) { throw "$state $verdict" }

    # The owner keeps only READ_CONTROL: whoever created the directory first must not keep
    # WRITE_DAC. OWNER RIGHTS (S-1-3-4) with nothing beyond reading the list.
    $ownerRights = @((Get-Acl $state).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
                     Where-Object { $_.IdentityReference.Value -eq 'S-1-3-4' -and $_.AccessControlType -eq 'Allow' -and
                                    -not ($_.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) })
    $beyondReading = (-bnot ([int64]$rights::ReadPermissions -bor [int64]$rights::Synchronize)) -band $lowWord
    if ($ownerRights.Count -eq 0) { throw "$state carries no OWNER RIGHTS entry, so its owner keeps WRITE_DAC" }
    if (@($ownerRights | Where-Object { ((([int64]$_.FileSystemRights) -band $lowWord) -band $beyondReading) -ne 0 }).Count -gt 0) {
        throw "$state lets its owner do more than read the access list: $($ownerRights.FileSystemRights)"
    }

    $serviceSid = (New-Object Security.Principal.NTAccount 'NT SERVICE\FastCacheCompileNode').Translate([Security.Principal.SecurityIdentifier]).Value
    if ($verdict = Get-NodeServiceAccessVerdict $state $key $serviceSid) { throw "NT SERVICE\FastCacheCompileNode: $verdict" }
    Write-Host "$state and its identity key answer to nobody but SYSTEM, Administrators and the service"
}

# ---------------------------------------------------------------------------
# msiexec and its log
# ---------------------------------------------------------------------------

# The lines of a verbose log that say what FAILED, each with the lines just before it: the error
# Windows Installer reports for an action, whatever form it takes. A deferred action that could not
# even be STARTED is logged as `Error 1721. ... Action: <name>, location: <its directory>`, which
# names no CustomAction and returns no code, so a filter for either missed the failure that ended
# the 0.3.0 upgrade (round 5). A pure function over the lines, so the self-test drives it.
#
# An action marked Return="ignore" that returned non-zero is logged as `returned actual error code
# N but will be translated to success` -- and is NOT a failure: a `reg delete` of a value that is not
# there is the ordinary case. Counted as one, eight of them stood ahead of the node's failed start in
# round 6's log, and the one line that ended the transaction read as the ninth. They still appear
# under "the package actions", and as context when they stand just before a real failure.
#
# @param Lines The log's lines.
# @param Before How many lines before each failure to keep.
# @return The failure lines and their context, in log order, each line once.
function Get-MsiFailureLines([string[]] $Lines, [int] $Before = 3) {
    $pattern = 'returned actual error code|Error 1[0-9]{3}\b|Return value 3\b|Error in rollback|Installation failed|failed to (start|run)'
    $ignored = 'will be translated to success'
    $keep = [System.Collections.Generic.SortedSet[int]]::new()
    foreach ($index in 0..($Lines.Count - 1)) {
        if ($Lines[$index] -match $pattern -and $Lines[$index] -notmatch $ignored) {
            foreach ($context in ([Math]::Max(0, $index - $Before))..$index) { [void] $keep.Add($context) }
        }
    }
    return @($keep | ForEach-Object { $Lines[$_] })
}

# The lines of a verbose log that name what happened, since its tail is only the
# property dump.
#
# Every line goes to the HOST, never to the output stream. It wrote its lines to the output stream
# until PR 1634's CI printed all three section headers over EMPTY sections, for a log a Select-String
# had just read: called from Invoke-TransactionJudgement, whose output Invoke-Msiexec captures in
# `Write-Host (Invoke-TransactionJudgement ...)`, the lines were captured, and the judgement's throw
# discarded them while the headers, already written to the host, survived. The self-test asserts this
# function writes nothing to the output stream.
#
# @param Path The log.
function Show-MsiLog([string] $Path) {
    Write-Host "===== $Path (relevant lines) ====="
    if (-not (Test-Path -LiteralPath $Path)) { Write-Host "no log at $Path"; return }
    $lines = @(Get-Content -LiteralPath $Path)
    # What failed, FIRST and from the whole log: the tail below fills with the rollback's own
    # records, which pushed the one error line out of it.
    Write-Host '--- what failed ---'
    foreach ($line in @(Get-MsiFailureLines $lines)) { Write-Host $line }
    # Every line naming one of this package's actions, from the whole log: the tail below is the
    # property dump of a transaction that ended, and an action that ran with Return="ignore" -- the
    # node's registration among them -- leaves its failure only here.
    Write-Host '--- the package actions ---'
    foreach ($line in @($lines -match 'FastCache\w+' -match 'Action (start|ended)|returned actual error|CustomAction|Error 1[0-9]{3}')) { Write-Host $line }
    Write-Host '--- the tail ---'
    $tail = 'Action (start|ended)|CustomAction|ServiceControl|FastCache|returned actual error|Note: 1: 1(4|7)[0-9][0-9]|Installation (success|failed)|error'
    foreach ($line in @($lines -match $tail | Select-Object -Last 60)) { Write-Host $line }
}

# How many RESTART MANAGER lines of one verbose log are printed. A transaction with a Restart Manager
# session logs a handful (opened, the shutdown mode, each application it shuts down or restarts,
# closed), and 0.3.0's nested removal logs its own after the outer package's, so the cap stands well
# above what one upgrade writes; a log that exceeds it says so rather than ending silently.
$script:RestartManagerLineCap = 100

# The RESTART MANAGER lines of a verbose log, every one up to @p Cap. PR 1634's first print took the
# first FOUR, which are the outer package's own lines, and cut 0.3.0's nested session that the print
# was cited for. A pure function over the lines.
#
# @param Lines The log's lines.
# @param Cap The most lines returned.
# @return Lines, the lines (trimmed, in log order), and Omitted, how many matching lines the cap cut.
function Get-MsiRestartManagerLines([string[]] $Lines, [int] $Cap = $script:RestartManagerLineCap) {
    $all = @($Lines -match 'RESTART MANAGER' | ForEach-Object { $_.Trim() })
    return [pscustomobject]@{ Lines = @($all | Select-Object -First $Cap); Omitted = [Math]::Max(0, $all.Count - $Cap) }
}

# Prints a verbose log's RESTART MANAGER lines, each behind @p Prefix, and says when the cap cut some.
#
# @param Path The log.
# @param Prefix Put before each line.
function Show-MsiRestartManagerLines([string] $Path, [string] $Prefix = '  log: ') {
    if (-not (Test-Path -LiteralPath $Path)) { Write-Host "$Prefix$Path was not written"; return }
    $found = Get-MsiRestartManagerLines @(Get-Content -LiteralPath $Path)
    if ($found.Lines.Count -eq 0) { Write-Host "${Prefix}no line names RESTART MANAGER"; return }
    foreach ($line in $found.Lines) { Write-Host "$Prefix$line" }
    if ($found.Omitted -gt 0) { Write-Host "${Prefix}TRUNCATED: $($found.Omitted) more RESTART MANAGER line(s) past the cap of $script:RestartManagerLineCap" }
}

# The time of day each line of a verbose log was written, carried forward to the lines that state
# none (a property dump, a continuation). Windows Installer stamps its own records
# `MSI (s) (D8:AC) [13:31:46:577]:` and an action's `Action start 13:31:46: <name>.` in the HOST's
# local time and without a date, so a time of day is all a line can be placed by. A pure function.
#
# @param Lines The log's lines.
# @return One TimeSpan per line, or $null for the lines before the first stamp.
function Get-MsiLogTimeOfDay([string[]] $Lines) {
    $current = $null
    foreach ($line in $Lines) {
        if ($line -match '\[(\d{1,2}):(\d{2}):(\d{2}):(\d{3})\]:') {
            $current = [TimeSpan]::new(0, [int] $Matches[1], [int] $Matches[2], [int] $Matches[3], [int] $Matches[4])
        } elseif ($line -match '^Action (start|ended) (\d{1,2}):(\d{2}):(\d{2}):') {
            $current = [TimeSpan]::new([int] $Matches[2], [int] $Matches[3], [int] $Matches[4])
        }
        , $current
    }
}

# What a verbose log was doing around each of @p Times: the actions starting and ending, the service
# control operations, the custom actions, the product's messages, the start of a nested product
# (`Running product`, how RemoveExistingProducts' removal of the old product opens in the same log),
# and every RESTART MANAGER line, from @p BeforeSeconds before a time to @p AfterSeconds after it.
# The windows of findings close together merge, and each line is returned once, in log order. A pure
# function over the lines, so the self-test drives it.
#
# @param Lines The log's lines.
# @param Times The instants, as the log's clock reads them: the host's LOCAL time of day.
# @param BeforeSeconds How far before each instant a line may be.
# @param AfterSeconds How far after it.
# @param Cap The most lines returned.
# @return Lines and Omitted, as Get-MsiRestartManagerLines'.
function Get-MsiLinesAround {
    param(
        [string[]] $Lines = @(),
        [TimeSpan[]] $Times = @(),
        [int] $BeforeSeconds = 30,
        [int] $AfterSeconds = 5,
        [int] $Cap = 200
    )
    $interesting = 'Action (start|ended)|Doing action:|Executing op: (ServiceControl|ActionStart|CustomAction)|ServiceControl|CustomAction|Product:|Running product|RESTART MANAGER|Windows Installer (installed|removed|reconfigured)'
    $clock = @(Get-MsiLogTimeOfDay $Lines)
    $day = [TimeSpan]::FromDays(1).Ticks
    $kept = @(foreach ($index in @(0..($Lines.Count - 1) | Where-Object { $Lines.Count -gt 0 })) {
            if ($null -eq $clock[$index] -or $Lines[$index] -notmatch $interesting) { continue }
            foreach ($time in $Times) {
                # Signed distance on a 24-hour circle, so a window across midnight still matches.
                $ticks = (($clock[$index].Ticks - $time.Ticks) % $day + $day + $day / 2) % $day - $day / 2
                if ($ticks -ge -[TimeSpan]::FromSeconds($BeforeSeconds).Ticks -and $ticks -le [TimeSpan]::FromSeconds($AfterSeconds).Ticks) {
                    $Lines[$index].Trim()
                    break
                }
            }
        })
    return [pscustomobject]@{ Lines = @($kept | Select-Object -First $Cap); Omitted = [Math]::Max(0, $kept.Count - $Cap) }
}

# What a transaction's verbose log says around its findings: every RESTART MANAGER line, then the
# actions around each finding's time. Shown when a judgement refuses, because a finding names an
# instant and the log alone says what Windows Installer was doing then.
#
# @param Path The log.
# @param Findings Get-TransactionServiceVerdict's records, each with a UTC Time.
function Show-MsiLogAroundFindings([string] $Path, [object[]] $Findings) {
    Write-Host "===== $Path around the findings ====="
    if (-not (Test-Path -LiteralPath $Path)) { Write-Host "no log at $Path"; return }
    $lines = @(Get-Content -LiteralPath $Path)
    Write-Host "--- every RESTART MANAGER line (at most $script:RestartManagerLineCap) ---"
    Show-MsiRestartManagerLines -Path $Path -Prefix ''
    $times = @($Findings | ForEach-Object { $_.Time.ToLocalTime().TimeOfDay } | Sort-Object -Unique)
    $around = Get-MsiLinesAround -Lines $lines -Times $times
    Write-Host "--- the actions from 30 s before to 5 s after each finding (local $(@($times | ForEach-Object { $_.ToString('hh\:mm\:ss\.fff') }) -join ', ')) ---"
    foreach ($line in $around.Lines) { Write-Host $line }
    if ($around.Lines.Count -eq 0) { Write-Host 'no action, service operation or Restart Manager line in those windows' }
    if ($around.Omitted -gt 0) { Write-Host "TRUNCATED: $($around.Omitted) more line(s) in those windows" }
}

# Who listens on @p Port and which of this package's service processes are alive: the holder of
# the node's port is the first thing a failed start needs named (batch 4 review, B4-2), and nothing
# on the runner survives the job to ask later. Never throws: it runs on the way to a throw that
# matters more.
#
# @param Port The TCP port.
function Show-PortHolders([int] $Port) {
    Write-Host "===== who holds $Port, and the service processes ====="
    try {
        foreach ($listener in @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)) {
            $owner = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
            Write-Host "$($listener.LocalAddress):$Port held by PID $($listener.OwningProcess): $($owner.Path)"
        }
        foreach ($process in @(Get-Process -Name fastcached, fastcache-compile-node -ErrorAction SilentlyContinue)) {
            Write-Host "running: PID $($process.Id) $($process.Path), started $($process.StartTime)"
        }
    } catch {
        Write-Host "the listing itself failed: $_"
    }
}

# Runs one silent, verbosely logged msiexec transaction and refuses an exit code
# it was not told to accept, showing the log first.
#
# @param Operation '/i' or '/x'.
# @param Package The .msi file.
# @param Log The verbose log, resolved against the current location.
# @param Properties PROPERTY=value arguments.
# @param Accept The exit codes that count as success.
# @param Notice Exit code to a sentence, emitted as a workflow notice when that
#               accepted code is the one returned: an accepted code can still be
#               something a reader of the run must be told.
# @param What The transaction, for the messages.
# @param Leaves The service table row the transaction leaves. It is JUDGED, inside this call:
#               see Get-TransactionExpectation. Exactly one of Leaves and Expect is given.
# @param Expect A named expectation of $script:TransactionExpectations, for a transaction that
#               cannot honestly state a row (one that fails and rolls back); its reason says why.
# @param Start Takes the arguments and returns the started msiexec Process; the self-test's seam, so
#              a neutered refusal starts a harmless process there rather than msiexec.
function Invoke-Msiexec {
    param(
        [Parameter(Mandatory)] [ValidateSet('/i', '/x')] [string] $Operation,
        [Parameter(Mandatory)] [string] $Package,
        [Parameter(Mandatory)] [string] $Log,
        [string[]] $Properties = @(),
        [int[]] $Accept = @(0),
        [hashtable] $Notice = @{},
        [Parameter(Mandatory)] [string] $What,
        [string] $Leaves = '',
        [string] $Expect = '',
        [scriptblock] $Start = { param($arguments) Start-Process msiexec.exe -PassThru -ArgumentList $arguments }
    )
    # Judged, always: the expectation is resolved, and refused by name, before anything runs. Neither
    # parameter is Mandatory, because a missing Mandatory parameter PROMPTS, and a prompt on a CI
    # runner's pwsh is a hang rather than a refusal.
    $expectation = Get-TransactionExpectation -Leaves $Leaves -Expect $Expect
    $logPath = [IO.Path]::GetFullPath($Log, (Get-Location).Path)
    $arguments = @($Operation, "`"$Package`"", '/qn', '/l*v', "`"$logPath`"") + $Properties
    Write-Host "$What`: msiexec $($arguments -join ' ')"
    # What each service ran as when the transaction began, then the transaction WATCHED rather than
    # waited for: a start nobody asked for (PR 1634's, inferred to be Restart Manager's) can live and die inside the
    # transaction, and only an observation taken while it runs can see it.
    $baseline = @{}
    foreach ($name in $script:ServiceDisplayNames.Keys) {
        $seen = Get-ServiceObservation $name
        $baseline[$name] = if ($null -eq $seen) { 0 } else { $seen.ProcessId }
    }
    $startedUtc = [DateTime]::UtcNow
    $p = & $Start $arguments
    # Held now: Start-Process -PassThru without -Wait hands back a Process whose ExitCode can read
    # $null once the process has exited unless its handle was taken first, which -Wait used to do.
    # The self-test's watch case reads an exit code through exactly this shape.
    $null = $p.Handle
    $watch = Watch-ServiceProcesses -Process $p
    $script:LastMsiTransaction = [pscustomobject]@{
        What = $What; Log = $logPath; StartedUtc = $startedUtc; Baseline = $baseline; Expectation = $expectation
        Polls = $watch.Polls; FailedPolls = $watch.FailedPolls; FirstFailure = $watch.FirstFailure; Observed = $watch.Observed
        Extended = $false
    }
    if ($p.ExitCode -notin $Accept) {
        Show-MsiLog $logPath
        # A checked service action that failed rolled the transaction back, and the service's
        # own events are where its refusal was written: they outlive the rollback.
        foreach ($name in 'FastCacheCompileNode', 'FastCached') { Show-ServiceDiagnosis $name }
        Show-PortHolders 6674
        throw "$What exited $($p.ExitCode); accepted: $($Accept -join ', ')"
    }
    Write-Host "$What exited $($p.ExitCode)"
    if ($Notice.ContainsKey($p.ExitCode)) { Write-Host "::notice::$What exited $($p.ExitCode): $($Notice[$p.ExitCode])" }
    # What the verbose log says Restart Manager did, SHOWN and not judged: the line's wording is not
    # pinned anywhere this module can read, so a pattern asserted on it would be a guess. Every line,
    # up to the cap: the first four were the outer package's own, and cut a nested removal's session.
    Show-MsiRestartManagerLines -Path $logPath
    Write-Host (Invoke-TransactionJudgement -Transaction $script:LastMsiTransaction)
}

# Asserts which lines a verbose log carries and which it must not.
#
# A pattern that must be ABSENT proves nothing about a log that never recorded
# the part of the transaction it is about, so a caller pairs each absence with a
# presence from that same part.
#
# @param Path The log.
# @param Present Patterns each at least one line must match.
# @param Absent Patterns no line may match.
function Assert-MsiLog {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [string[]] $Present = @(),
        [string[]] $Absent = @()
    )
    foreach ($pattern in $Present) {
        if (-not (Select-String -Path $Path -Pattern $pattern -Quiet)) {
            Show-MsiLog $Path
            throw "$Path has no line matching '$pattern'"
        }
    }
    foreach ($pattern in $Absent) {
        $hits = @(Select-String -Path $Path -Pattern $pattern)
        if ($hits.Count -gt 0) {
            $hits | ForEach-Object { Write-Host $_.Line }
            throw "$Path has $($hits.Count) line(s) matching '$pattern'"
        }
    }
}

# Whether @p Actions each RAN, in that order, in a verbose log: the first record of each, in
# the order given. A pure verdict over the lines, so the self-test drives it without a
# transaction. An upgrade's log holds the old product's session too, so an action both
# products name is found at its FIRST run -- the old product's, which is the point when the
# question is whether the old registration was gone before this product made its own.
#
# @param Lines The log's lines.
# @param Actions The actions, in the order they must have run.
# @return $null when each ran, in order; else which did not, or which ran out of order.
function Get-MsiActionOrderVerdict([string[]] $Lines, [string[]] $Actions) {
    $previous = -1
    $previousAction = ''
    foreach ($action in $Actions) {
        $pattern = Get-MsiActionRanPattern $action
        $at = -1
        foreach ($index in 0..($Lines.Count - 1)) {
            if ($Lines[$index] -match $pattern) { $at = $index; break }
        }
        if ($at -lt 0) { return "$action never ran" }
        if ($at -le $previous) { return "$action ran (line $($at + 1)) before $previousAction (line $($previous + 1))" }
        $previous = $at
        $previousAction = $action
    }
    return $null
}

# @p Actions each ran, in that order, in the verbose log at @p Path. An unreadable log throws.
function Assert-MsiActionOrder([string] $Path, [string[]] $Actions) {
    $lines = @(Get-Content -Path $Path -ErrorAction Stop)
    if ($verdict = Get-MsiActionOrderVerdict $lines $Actions) {
        Show-MsiLog $Path
        throw "${Path}: $verdict"
    }
    Write-Host "$Path ran $($Actions -join ', then ')"
}

# The pattern for a verbose log's record that an action RAN: its condition held.
# One whose condition is false is logged as "Skipping action" instead.
#
# @param Action The action name.
# @return The pattern.
function Get-MsiActionRanPattern([string] $Action) {
    return "Doing action: $([regex]::Escape($Action))\s*$"
}

# ---------------------------------------------------------------------------
# What a transaction did to the services (PR 1634)
# ---------------------------------------------------------------------------
#
# The service table row a transaction leaves is asserted AFTER it, so a service
# that was started during it and died again -- or crash-looped, stopped between
# two recovery restarts at the moment it was asked -- passes the row. That is
# what PR 1634's CI showed (MEASURED): the upgrade from 0.3.0 started FastCached
# at 12:12:47, after the table had made it manual and stopped it for the node;
# it could not bind the port the node holds, terminated three times before
# msiexec returned, and the row read "Manual and Stopped". That Windows
# Installer's Restart Manager started it is INFERRED: nothing in the package
# starts FastCached on that path, and FastCached stopped one second into the
# transaction, at InstallValidate time.
#
# So Invoke-Msiexec JUDGES every transaction it runs, against the row it names
# (`-Leaves`) or a named expectation (`-Expect`), and Assert-ServiceTable judges
# the same transaction again over the longer window that reaches its own
# stability interval. Three witnesses, each a pure function over records:
#
#   * the service control manager's unexpected-termination events (7031, 7034)
#     naming a service of this package since the transaction began;
#   * its "entered the running state" event (7036) for a service the
#     expectation leaves NOT RUNNING (stopped, or not registered);
#   * a process the watch saw such a service run under, other than the one it
#     ran under when the transaction began.
#
# 7036 is a witness only where the host writes it. MEASURED on two hosts: the
# Windows Server 2025 runner of PR 1634's CI writes it (its log printed "The
# FastCached service entered the running state"); this repository's Windows 11
# 26200 development host wrote none among 50,978 System records. So the watch
# stays, and each transaction that starts the node says which of the two hosts
# it ran on: the node's own start is the positive control (Get-Witness7036State).

# Expectations a transaction states when no row honestly describes it, each with
# its reason. A row is the default for everything else, and nothing defaults to
# one of these: Invoke-Msiexec refuses a call that names neither.
#
#   MayTerminate  the services whose unexpected termination this transaction is
#                 BUILT to cause; every other termination is still a finding.
#
# Neither row watches for a start: a rollback starts again what the transaction
# stopped, and removes what it created and started, so which service may start
# during it is the rollback's business, not a row's.
$script:TransactionExpectations = [ordered]@{
    RollsBack = @{
        MayTerminate = @()
        Reason = 'the transaction fails on purpose and rolls back: the rollback restarts what it stopped and removes what it created, so no row describes what may start during it; every unexpected termination is still a finding'
    }
    NodeCannotBind = @{
        MayTerminate = @('FastCacheCompileNode')
        Reason = 'the step holds 6674, so the node the transaction adds cannot bind and its refusal is logged as an unexpected termination (7034, measured in PR 1634); the transaction then rolls back, so no start is judged; a termination of FastCached is still a finding'
    }
}

# What a transaction is judged against: a service table row (`-Leaves`), or a named expectation
# (`-Expect`). Exactly one; neither, both, or an unknown name is refused by name.
#
# @param Leaves A key of $script:ServiceTable, or empty.
# @param Expect A key of $script:TransactionExpectations, or empty.
# @return Name; NotRunning, the services that must not be STARTED during it (a row's Stopped and
#         unregistered services); MayTerminate; NodeStarts, whether the transaction starts the node
#         (the 7036 positive control); and Reason.
function Get-TransactionExpectation([string] $Leaves = '', [string] $Expect = '') {
    if ([bool] $Leaves -eq [bool] $Expect) {
        throw "Invoke-Msiexec judges every transaction: name the service table row it -Leaves ($($script:ServiceTable.Keys -join ', ')), or, for a transaction no row describes, a named -Expect ($($script:TransactionExpectations.Keys -join ', ')); exactly one"
    }
    if ($Leaves) {
        if (-not $script:ServiceTable.Contains($Leaves)) {
            throw "no service table row '$Leaves'; the rows are: $($script:ServiceTable.Keys -join ', ')"
        }
        $row = $script:ServiceTable[$Leaves]
        $notRunning = @(foreach ($name in $row.Keys) { if ($null -eq $row[$name] -or $row[$name][1] -eq 'Stopped') { $name } })
        $node = $row['FastCacheCompileNode']
        return [pscustomobject]@{
            Name = "row $Leaves"; NotRunning = $notRunning; MayTerminate = @()
            NodeStarts = ($null -ne $node -and $node[1] -eq 'Running'); Reason = ''
        }
    }
    if (-not $script:TransactionExpectations.Contains($Expect)) {
        throw "no transaction expectation '$Expect'; the expectations are: $($script:TransactionExpectations.Keys -join ', ')"
    }
    $named = $script:TransactionExpectations[$Expect]
    return [pscustomobject]@{
        Name = "expectation $Expect"; NotRunning = @(); MayTerminate = @($named.MayTerminate); NodeStarts = $false; Reason = $named.Reason
    }
}

# Every process a service of this package runs under while @p Process runs, observed every
# @p IntervalMilliseconds until it exits.
#
# An observation that FAILS is counted and the watch goes on, and the process is waited for on every
# way out: an error escaping here would leave msiexec running, untracked, under a step that had
# already thrown.
#
# @param Process The running process, its handle already taken.
# @param Observe Takes a service name and returns what Get-ServiceObservation would.
# @param IntervalMilliseconds The interval between observations.
# @return Polls, the rounds in which every observation succeeded; FailedPolls, the rounds in which one
#         failed, and FirstFailure, its error; and Observed, one record per sighting of a service
#         running under a process (Time in UTC, Service, State, ProcessId).
function Watch-ServiceProcesses {
    param(
        [Parameter(Mandatory)] [System.Diagnostics.Process] $Process,
        [scriptblock] $Observe = { param($name) Get-ServiceObservation $name },
        [int] $IntervalMilliseconds = 250
    )
    $observed = [System.Collections.Generic.List[object]]::new()
    $polls = 0
    $failedPolls = 0
    $firstFailure = ''
    try {
        while (-not $Process.WaitForExit($IntervalMilliseconds)) {
            $failed = $false
            foreach ($name in $script:ServiceDisplayNames.Keys) {
                try {
                    $seen = & $Observe $name
                } catch {
                    $failed = $true
                    if (-not $firstFailure) { $firstFailure = "$name`: $($_.Exception.Message)" }
                    continue
                }
                if ($null -ne $seen -and $seen.ProcessId -gt 0) {
                    $observed.Add([pscustomobject]@{ Time = [DateTime]::UtcNow; Service = $name; State = $seen.State; ProcessId = $seen.ProcessId })
                }
            }
            if ($failed) { $failedPolls++ } else { $polls++ }
        }
    } finally {
        $Process.WaitForExit()
    }
    return [pscustomobject]@{ Polls = $polls; FailedPolls = $failedPolls; FirstFailure = $firstFailure; Observed = $observed.ToArray() }
}

# One service control manager event as a record, from its XML and its rendered message.
#
# The service is read from the first of these that the event's shape carries:
#   1. its binary data, the SERVICE name in UTF-16 (a real 7031's decodes to FastCacheCompileNode); a
#      `/` in it is read as `<name>/<state code>`, a layout seen reported for 7036 but NOT verified
#      here, so it decides only when param2 says nothing;
#   2. a `ServiceName` data item, as 7045 ("a service was installed") carries it;
#   3. param1, the display name.
# Either name of a service of this package maps to its service name through ServiceDisplayNames; any
# other is kept as it was. A 7036's state is its param2 ("running", "stopped"), the form Microsoft's
# own event queries match on.
#
# @param Xml The event's XML (EventLogRecord.ToXml()).
# @param Message The event's rendered message.
# @return Id, Time (UTC), Service, State ('Running', 'Stopped' or '' when the event states none) and
#         Message.
function ConvertTo-ServiceControlEvent([string] $Xml, [string] $Message) {
    [xml] $document = $Xml
    $namespace = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
    $namespace.AddNamespace('e', 'http://schemas.microsoft.com/win/2004/08/events/event')
    $data = { param([string] $name)
        $node = $document.SelectSingleNode("/e:Event/e:EventData/e:Data[@Name='$name']", $namespace)
        if ($null -eq $node) { '' } else { $node.InnerText } }
    $id = [int] $document.SelectSingleNode('/e:Event/e:System/e:EventID', $namespace).InnerText
    $stamp = $document.SelectSingleNode('/e:Event/e:System/e:TimeCreated', $namespace).GetAttribute('SystemTime')
    $time = [DateTime]::Parse($stamp, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::RoundtripKind).ToUniversalTime()
    $binary = $document.SelectSingleNode('/e:Event/e:EventData/e:Binary', $namespace)
    $name = ''
    $code = ''
    if ($null -ne $binary -and $binary.InnerText.Length -ge 4 -and $binary.InnerText.Length % 2 -eq 0) {
        $hex = $binary.InnerText
        $bytes = [byte[]] @(foreach ($at in 0..($hex.Length / 2 - 1)) { [Convert]::ToByte($hex.Substring($at * 2, 2), 16) })
        $name = [Text.Encoding]::Unicode.GetString($bytes).TrimEnd([char] 0)
        if ($name.Contains('/')) { $name, $code = $name.Split('/', 2) }
    }
    if (-not $name) { $name = & $data 'ServiceName' }
    if (-not $name) { $name = & $data 'param1' }
    foreach ($service in $script:ServiceDisplayNames.Keys) {
        if ($name -eq $service -or $name -eq $script:ServiceDisplayNames[$service]) { $name = $service; break }
    }
    $state = ''
    if ($id -eq 7036) {
        $state = switch (& $data 'param2') { 'running' { 'Running' } 'stopped' { 'Stopped' } default {
                switch ($code.Trim([char] 0)) { '4' { 'Running' } '1' { 'Stopped' } default { '' } } } }
    }
    return [pscustomobject]@{ Id = $id; Time = $time; Service = $name; State = $state; Message = ($Message -replace '\s+', ' ').Trim() }
}

# The service control manager's events of @p Ids since @p SinceUtc, as records. An empty answer is
# NOTHING, not a failure: Get-WinEvent reports "no events were found" as an error, and only that
# error is taken as the answer; every other one throws.
#
# @param SinceUtc The earliest instant, in UTC.
# @param Ids The event ids.
# @param MaxEvents At most this many, newest first; 0 for all.
# @return One ConvertTo-ServiceControlEvent record per event; wrap the call in @() to count it.
function Get-ServiceControlEvents {
    param(
        [Parameter(Mandatory)] [datetime] $SinceUtc,
        [int[]] $Ids = $script:TransactionEvents,
        [int] $MaxEvents = 0
    )
    $filter = @{ LogName = 'System'; ProviderName = 'Service Control Manager'; Id = $Ids; StartTime = $SinceUtc.ToLocalTime() }
    $bound = if ($MaxEvents -gt 0) { @{ MaxEvents = $MaxEvents } } else { @{} }
    try {
        $records = @(Get-WinEvent -FilterHashtable $filter @bound -ErrorAction Stop)
    } catch {
        if ($_.FullyQualifiedErrorId -like 'NoMatchingEventsFound*') { return }
        throw
    }
    foreach ($record in $records) { ConvertTo-ServiceControlEvent $record.ToXml() $record.Message }
}

# The decision, apart from the acquisition: what a transaction did to the services that its
# expectation did not allow.
#
# @param What The transaction, for the findings.
# @param StartedUtc When it began; an event before it belongs to something else.
# @param Events ConvertTo-ServiceControlEvent records.
# @param Observed Watch-ServiceProcesses' Observed records.
# @param Baseline Service name to the process it ran under when the transaction began, 0 for none.
# @param NotRunning The services the expectation leaves not running: none may be STARTED.
# @param MayTerminate The services whose unexpected termination the expectation allows.
# @return One finding per unexpected termination and per start of a NotRunning service, each with a
#         Kind ('Termination' or 'Start'), its Time (UTC) and a Text naming the service, the event id or process, the
#         time and the message; NOTHING when there is none (wrap the call in @() to count it).
function Get-TransactionServiceVerdict {
    param(
        [Parameter(Mandatory)] [string] $What,
        [Parameter(Mandatory)] [datetime] $StartedUtc,
        [object[]] $Events = @(),
        [object[]] $Observed = @(),
        [hashtable] $Baseline = @{},
        [string[]] $NotRunning = @(),
        [string[]] $MayTerminate = @()
    )
    $format = { param([datetime] $t) $t.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ', [Globalization.CultureInfo]::InvariantCulture) }
    $finding = { param([string] $kind, [datetime] $time, [string] $text) [pscustomobject]@{ Kind = $kind; Time = $time; Text = $text } }
    $ours = @($Events | Where-Object { $_.Time -ge $StartedUtc -and $script:ServiceDisplayNames.Contains($_.Service) } | Sort-Object Time)
    foreach ($record in $ours) {
        if ($record.Id -notin $script:UnexpectedTerminationEvents -or $record.Service -in $MayTerminate) { continue }
        & $finding 'Termination' $record.Time "$($record.Service) terminated unexpectedly during '$What': event $($record.Id) at $(& $format $record.Time): $($record.Message)"
    }
    foreach ($name in $NotRunning) {
        foreach ($record in @($ours | Where-Object { $_.Id -eq 7036 -and $_.Service -eq $name -and $_.State -eq 'Running' })) {
            & $finding 'Start' $record.Time "$name, which the expectation leaves not running, was started during '$What': event 7036 at $(& $format $record.Time): $($record.Message)"
        }
        $before = if ($Baseline.ContainsKey($name)) { [int] $Baseline[$name] } else { 0 }
        $strays = @($Observed | Where-Object { $_.Service -eq $name -and $_.ProcessId -gt 0 -and $_.ProcessId -ne $before } |
                Sort-Object Time | Group-Object ProcessId)
        foreach ($stray in $strays) {
            $first = $stray.Group[0]
            $was = if ($before -gt 0) { "it ran as process $before when the transaction began" } else { 'it was not running when the transaction began' }
            & $finding 'Start' $first.Time "$name, which the expectation leaves not running, was started during '$What': process $($first.ProcessId) seen $($first.State) at $(& $format $first.Time), $($stray.Count) time(s); $was"
        }
    }
}

# Whether the 7036 witness is LIVE in a transaction, from its one positive control: the node's own
# start. A host that writes 7036 logs it whenever the node starts, and every transaction whose
# expectation leaves the node running starts (or restarts) it.
#
# @param Events ConvertTo-ServiceControlEvent records.
# @param StartedUtc When the transaction began.
# @param NodeStarts Whether the transaction starts the node.
# @return 'live' (the node's start was logged), 'absent' (the node started and no 7036 says so: this
#         host does not write them, and the watch is the only start witness) or 'no control' (the
#         transaction starts no node, so nothing here can tell).
function Get-Witness7036State([object[]] $Events = @(), [datetime] $StartedUtc, [bool] $NodeStarts) {
    if (-not $NodeStarts) { return 'no control' }
    $seen = @($Events | Where-Object { $_.Id -eq 7036 -and $_.Time -ge $StartedUtc -and $_.Service -eq 'FastCacheCompileNode' -and $_.State -eq 'Running' })
    if ($seen.Count -gt 0) { return 'live' }
    return 'absent'
}

# Judges @p Transaction against its expectation, over every event since it began: refuses with each
# finding, or returns the line that says what was judged, including which witnesses were live.
#
# @param Transaction The record Invoke-Msiexec keeps.
# @param ReadEvents Takes the transaction's start in UTC and returns ConvertTo-ServiceControlEvent
#                   records; the seam through which the self-test supplies them.
# @return The pass line.
function Invoke-TransactionJudgement {
    param(
        [Parameter(Mandatory)] $Transaction,
        [scriptblock] $ReadEvents = { param($since) Get-ServiceControlEvents -SinceUtc $since }
    )
    $expectation = $Transaction.Expectation
    # A watch that never looked reports no start, which is not the same as none happening.
    if ($Transaction.Polls -lt 1) {
        throw "the services were never observed while '$($Transaction.What)' ran ($($Transaction.FailedPolls) observation round(s) failed, first: $($Transaction.FirstFailure)), so 'nothing was started' would describe nothing"
    }
    $events = @(& $ReadEvents $Transaction.StartedUtc)
    $findings = @(Get-TransactionServiceVerdict -What $Transaction.What -StartedUtc $Transaction.StartedUtc -Events $events `
            -Observed $Transaction.Observed -Baseline $Transaction.Baseline -NotRunning $expectation.NotRunning -MayTerminate $expectation.MayTerminate)
    if ($findings.Count -gt 0) {
        if ($Transaction.Log) {
            Show-MsiLog $Transaction.Log
            Show-MsiLogAroundFindings -Path $Transaction.Log -Findings $findings
        }
        Show-PortHolders 6674
        $kinds = @($findings | ForEach-Object Kind | Sort-Object -Unique)
        $hints = @(
            if ('Termination' -in $kinds) { 'a TERMINATION is the service failing: read its time against the verbose log and the service''s own Application events (Show-ServiceDiagnosis), a refused start and a bad restore look like this' }
            if ('Start' -in $kinds) { 'a START of a service the expectation leaves not running has had one cause so far, Windows Installer''s Restart Manager (PR 1634), which the package disables (MSIRESTARTMANAGERCONTROL); read the verbose log''s RESTART MANAGER lines and the actions running at that time' }
        )
        throw "'$($Transaction.What)' disturbed a service the table owns ($($expectation.Name)):`n  $(@($findings | ForEach-Object Text) -join "`n  ")`n$($hints -join "`n")"
    }
    $witness = switch (Get-Witness7036State -Events $events -StartedUtc $Transaction.StartedUtc -NodeStarts $expectation.NodeStarts) {
        'live' { '7036 live (the node''s own start is logged)' }
        'absent' { '7036 ABSENT: the node started and no 7036 says so, so this host does not write them and the watch is the only start witness' }
        default { '7036 unchecked: the transaction starts no node to be its control' }
    }
    $watched = if ($expectation.NotRunning.Count) { "no start of $($expectation.NotRunning -join ', ')" } else { 'no start judged' }
    $gaps = if ($Transaction.FailedPolls -gt 0) { "; $($Transaction.FailedPolls) observation round(s) FAILED, first: $($Transaction.FirstFailure)" } else { '' }
    $why = if ($expectation.Reason) { " -- $($expectation.Reason)" } else { '' }
    return "  '$($Transaction.What)' judged against $($expectation.Name)$why`: no unexpected termination, $watched; $witness; $($Transaction.Polls) observation round(s)$gaps"
}

# ---------------------------------------------------------------------------
# Installation snapshots (#1629)
# ---------------------------------------------------------------------------

# Service registry values that change on their own between two observations of an UNCHANGED
# installation, with the reason each is excluded. Starts EMPTY on purpose: a row is added only with
# the CI log line that shows the value moving, never pre-emptively (#1629).
$script:VolatileServiceValues = @{}

# Every value under @p Path and its subkeys, one sorted line each, '<subkey>\<name> <kind> <data>',
# the data UNexpanded. '<absent>' alone when the key does not exist, so absence is a value that
# compares rather than an empty list that matches anything.
#
# A row of VolatileServiceValues is keyed '<service>\<label>', where <label> is the line's own first
# token: the value's name, behind its subkey path when it lives in a subkey ('Svc\Parameters\X').
#
# @param Path Key path relative to @p Hive.
# @param Service The service the key belongs to, to look its volatile values up; empty for none.
# @param Hive The hive @p Path is in: HKLM for every snapshot, HKCU for the self-test's scratch key.
# @return The lines, always an array.
function Get-RegistryTreeLines([string] $Path, [string] $Service = '', [Microsoft.Win32.RegistryKey] $Hive = [Microsoft.Win32.Registry]::LocalMachine) {
    $key = $Hive.OpenSubKey($Path)
    if ($null -eq $key) { return , @('<absent>') }
    try {
        $lines = [System.Collections.Generic.List[string]]::new()
        $walk = {
            param($k, [string] $prefix)
            foreach ($name in $k.GetValueNames()) {
                $label = if ($prefix) { "$prefix\$name" } else { $name }
                if ($Service -and $script:VolatileServiceValues.ContainsKey("$Service\$label")) { continue }
                $kind = $k.GetValueKind($name)
                $data = $k.GetValue($name, $null, 'DoNotExpandEnvironmentNames')
                $text = switch ($kind) {
                    'Binary' { ($data | ForEach-Object { $_.ToString('x2') }) -join '' }
                    'MultiString' { ($data | ForEach-Object { "`"$_`"" }) -join ',' }
                    default { "$data" }
                }
                $lines.Add("$label $kind $text")
            }
            foreach ($sub in $k.GetSubKeyNames()) {
                $child = $k.OpenSubKey($sub)
                try { & $walk $child $(if ($prefix) { "$prefix\$sub" } else { $sub }) } finally { $child.Dispose() }
            }
        }
        & $walk $key ''
        return , @($lines | Sort-Object)
    } finally { $key.Dispose() }
}

# Waits, bounded, until @p Name is in no *Pending state, then requires it to HOLD for @p StableSeconds,
# and returns what was seen: the state ('absent' when not registered), plus '; unstable: <verdict>'
# when the second observation disagrees with the first. A rollback restarts services as its last
# acts, so a reading taken at msiexec's exit can catch Start Pending; and one reading cannot tell a
# running service from a crash-looping one, so a crash loop must read differently from a steady
# Running. 30 s is Assert-ServiceState's own bound, measured on a monotonic clock.
#
# @param Name The service name.
# @param StableSeconds How long the settled state must hold before it is believed.
# @param Observe Takes a service name and returns what Get-ServiceObservation would.
function Get-SettledServiceState {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [int] $StableSeconds = 5,
        [scriptblock] $Observe = { param($name) Get-ServiceObservation $name }
    )
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        $seen = & $Observe $Name
        if ($null -eq $seen) { return 'absent' }
        if ($seen.State -notlike '*Pending') { break }
        if ($clock.Elapsed.TotalSeconds -ge 30) { throw "$Name still $($seen.State) after 30 s" }
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds $StableSeconds
    $verdict = Get-ServiceStabilityVerdict $Name $seen (& $Observe $Name) $StableSeconds
    if ($null -eq $verdict) { return $seen.State }
    return "$($seen.State); unstable: $verdict"
}

# Everything an installation consists of that a rollback must put back, as comparable lines.
#
# @param UpgradeCode The product family.
# @param Root The installation root.
# @return An ordered hashtable from field name to sorted string[].
function Get-InstallationSnapshot {
    param([Parameter(Mandatory)] [string] $UpgradeCode, [Parameter(Mandatory)] [string] $Root)
    $snapshot = [ordered]@{}
    foreach ($name in 'FastCached', 'FastCacheCompileNode') {
        $snapshot["service $name"] = Get-RegistryTreeLines "SYSTEM\CurrentControlSet\Services\$name" $name
        $snapshot["state $name"] = @(Get-SettledServiceState $name)
    }
    $snapshot['installer values'] = Get-RegistryTreeLines 'SOFTWARE\fastcached\Installer'
    $snapshot['rollback state'] = Get-RegistryTreeLines 'SOFTWARE\fastcached\InstallerRollback'
    foreach ($group in 'fastcached: FastCached', 'fastcached: FastCacheCompileNode') {
        $snapshot["firewall $group"] = @(Get-FirewallGroupSnapshot $group)
    }
    # Bound first: the reader returns its array as ONE pipeline object, so piping the call itself
    # would sort a single nested Object[] and compare it by reference.
    $codes = Get-InstalledProductCodes $UpgradeCode
    $snapshot['products'] = @($codes | Sort-Object)
    $snapshot['binary'] = @("fastcached.exe present: $(Test-Path -LiteralPath (Join-Path $Root 'bin\fastcached.exe'))")
    return $snapshot
}

# The named differences between two snapshots: a pure decision, so the self-test drives it.
#
# @param Before The earlier snapshot.
# @param After The later snapshot.
# @return One "<field>: lost [..]; gained [..]" per differing field, NOTHING when equal (wrap the call
#         in @() to count it). Throws, naming the field, on an element that is not a string.
function Compare-InstallationSnapshot {
    param([Parameter(Mandatory)] $Before, [Parameter(Mandatory)] $After)
    $fields = @($Before.Keys) + @($After.Keys | Where-Object { -not $Before.Contains($_) })
    foreach ($record in $Before, $After) {
        foreach ($field in $record.Keys) {
            $bad = @($record[$field] | Where-Object { $_ -isnot [string] } | ForEach-Object { if ($null -eq $_) { '$null' } else { $_.GetType().Name } })
            if ($bad.Count) { throw "snapshot field '$field' holds a non-string element ($($bad[0])): the instrument is broken, not the installation" }
        }
    }
    $out = foreach ($field in $fields) {
        $hadBefore = $Before.Contains($field)
        $hasAfter = $After.Contains($field)
        $b = @(if ($hadBefore) { $Before[$field] })
        $a = @(if ($hasAfter) { $After[$field] })
        # A multiset difference: a line held twice where it was held once is a change.
        $lost = @(Get-MultisetDifference $b $a)
        $gained = @(Get-MultisetDifference $a $b)
        $text = "${field}: lost [$($lost -join '; ')]; gained [$($gained -join '; ')]"
        # Presence is a value: an EMPTY field one side lacks would otherwise match anything.
        if ($hadBefore -ne $hasAfter -and -not ($lost.Count -or $gained.Count)) {
            "$text <field absent $(if ($hadBefore) { 'after' } else { 'before' })>"
        } elseif ($lost.Count -or $gained.Count) { $text }
    }
    # Enumerated, not wrapped: an empty answer is NOTHING, so a caller's @() counts it as zero.
    return $out
}

# A COPY of @p Package with @p Statements applied, each change read back through @p Verify
# (query -> expected value) before the copy is trusted. An UPDATE matching no row succeeds silently,
# so an unverified control copy can be the unchanged package -- a control that cannot go red.
#
# @param Package The source .msi, left untouched.
# @param Destination The patched copy.
# @param Statements MSI SQL run in order against the copy.
# @param Verify Query (first column of its first row) to the value it must read back.
function New-MsiControlCopy {
    param(
        [Parameter(Mandatory)] [string] $Package, [Parameter(Mandatory)] [string] $Destination,
        [Parameter(Mandatory)] [string[]] $Statements, [Parameter(Mandatory)] [hashtable] $Verify
    )
    Copy-Item -LiteralPath $Package -Destination $Destination -Force
    Set-ItemProperty -LiteralPath $Destination -Name IsReadOnly -Value $false
    $invoke = { param($o, [string] $m, [string] $k, $a) $o.GetType().InvokeMember($m, $k, $null, $o, $a) }
    $installer = New-Object -ComObject WindowsInstaller.Installer
    try {
        # [string]: COM refuses a wrapped path with DISP_E_TYPEMISMATCH. Mode 1 is transact.
        $db = & $invoke $installer 'OpenDatabase' 'InvokeMethod' @([string]$Destination, 1)
        try {
            foreach ($sql in $Statements) {
                $view = & $invoke $db 'OpenView' 'InvokeMethod' @($sql)
                try { & $invoke $view 'Execute' 'InvokeMethod' $null | Out-Null }
                finally {
                    & $invoke $view 'Close' 'InvokeMethod' $null | Out-Null
                    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
                }
            }
            & $invoke $db 'Commit' 'InvokeMethod' $null | Out-Null
        } finally {
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($db)
        }
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
    # The read-back is a fresh read-only open, after the writing handle is gone.
    foreach ($query in $Verify.Keys) {
        $read = Get-MsiScalar $Destination $query
        if ($null -eq $read) { $read = '<no row>' }
        if ($read -cne $Verify[$query]) { throw "${Destination}: '$query' read '$read', expected '$($Verify[$query])'" }
    }
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

# Drives everything above that can be driven without installing a package, in
# both directions, and says how many cases it ran.
function Invoke-MsiServiceTableSelfTest {
    $script:SelfTestCases = 0
    function Pass([string] $case) { $script:SelfTestCases++; Write-Host "ok   $case" }
    function ExpectThrow([string] $case, [scriptblock] $body, [string] $pattern) {
        try { & $body } catch {
            if ($_.Exception.Message -match $pattern) { Pass $case; return }
            throw "$case`: threw '$($_.Exception.Message)', expected a message matching '$pattern'"
        }
        throw "$case`: did not throw; expected a message matching '$pattern'"
    }
    function Seen([string] $mode, [string] $state, [int] $processId = 0) {
        [pscustomobject]@{ StartMode = $mode; State = $state; ProcessId = $processId }
    }

    # The verdict, over synthetic observations: every outcome, and WHICH message.
    $verdicts = @(
        @{ Case = 'matching mode and state'; Want = @('Auto', 'Running'); Seen = (Seen 'Auto' 'Running'); Match = $null }
        @{ Case = 'wrong start mode'; Want = @('Auto', 'Running'); Seen = (Seen 'Manual' 'Running'); Match = "registered 'Manual', expected 'Auto'" }
        @{ Case = 'wrong state'; Want = @('Manual', 'Stopped'); Seen = (Seen 'Manual' 'Running'); Match = 'is Running, expected Stopped' }
        @{ Case = 'a pending state is not the state'; Want = @('Auto', 'Running'); Seen = (Seen 'Auto' 'Start Pending'); Match = 'is Start Pending, expected Running' }
        @{ Case = 'expected and not registered'; Want = @('Auto', 'Running'); Seen = $null; Match = 'is not registered' }
        @{ Case = 'absent as expected'; Want = $null; Seen = $null; Match = $null }
        @{ Case = 'registered where none may be'; Want = $null; Seen = (Seen 'Auto' 'Stopped'); Match = 'still registered \(Auto, Stopped\)' }
    )
    foreach ($row in $verdicts) {
        $verdict = Get-ServiceVerdict 'Svc' $row.Want $row.Seen
        if ($null -eq $row.Match) {
            if ($null -ne $verdict) { throw "$($row.Case): expected no verdict, got '$verdict'" }
        } elseif ($null -eq $verdict -or $verdict -notmatch $row.Match) {
            throw "$($row.Case): expected a verdict matching '$($row.Match)', got '$verdict'"
        }
        Pass "verdict: $($row.Case)"
    }

    # The stability verdict, over synthetic pairs: a crash loop reads Running at
    # both observations and only its process id tells it apart.
    $stability = @(
        @{ Case = 'the same process twice'; First = (Seen 'Auto' 'Running' 40); Second = (Seen 'Auto' 'Running' 40); Match = $null }
        @{ Case = 'a stopped service stays stopped'; First = (Seen 'Manual' 'Stopped' 0); Second = (Seen 'Manual' 'Stopped' 0); Match = $null }
        @{ Case = 'Running twice under two processes'; First = (Seen 'Auto' 'Running' 40); Second = (Seen 'Auto' 'Running' 41); Match = 'restarted within 5 s \(process 40 became 41\)' }
        @{ Case = 'Running then stopped'; First = (Seen 'Auto' 'Running' 40); Second = (Seen 'Auto' 'Stopped' 0); Match = 'went from Running to Stopped' }
        @{ Case = 'start mode changed'; First = (Seen 'Auto' 'Running' 40); Second = (Seen 'Manual' 'Running' 40); Match = 'changed start mode from Auto to Manual' }
        @{ Case = 'deregistered'; First = (Seen 'Auto' 'Running' 40); Second = $null; Match = 'was deregistered within 5 s' }
        @{ Case = 'Running with no process'; First = (Seen 'Auto' 'Running' 0); Second = (Seen 'Auto' 'Running' 0); Match = 'Running with no process id \(process 0, then 0\)' }
    )
    foreach ($row in $stability) {
        $verdict = Get-ServiceStabilityVerdict 'Svc' $row.First $row.Second 5
        if ($null -eq $row.Match) {
            if ($null -ne $verdict) { throw "$($row.Case): expected no verdict, got '$verdict'" }
        } elseif ($null -eq $verdict -or $verdict -notmatch $row.Match) {
            throw "$($row.Case): expected a verdict matching '$($row.Match)', got '$verdict'"
        }
        Pass "stability: $($row.Case)"
    }

    # The same decision WIRED: the waiting assertion must take the second
    # observation and ask the verdict, which the pairs above cannot show. A
    # scripted observer answers Running twice, under one process or two.
    foreach ($row in @(
            @{ Case = 'wiring: a restart between the two observations is refused'; Ids = 40, 41; Match = 'restarted within 0 s \(process 40 became 41\)' }
            @{ Case = 'wiring: one process at both observations passes'; Ids = 40, 40; Match = $null })) {
        $queue = [Collections.Generic.Queue[object]]::new()
        foreach ($id in $row.Ids) { $queue.Enqueue((Seen 'Auto' 'Running' $id)) }
        $observe = { param($name) $queue.Dequeue() }.GetNewClosure()
        $expect = @{ Svc = @('Auto', 'Running') }
        if ($null -eq $row.Match) {
            Assert-ServiceState -Expect $expect -TimeoutSeconds 1 -StableSeconds 0 -Observe $observe
            if ($queue.Count -ne 0) { throw "$($row.Case): the second observation was never taken" }
            Pass $row.Case
        } else {
            ExpectThrow $row.Case { Assert-ServiceState -Expect $expect -TimeoutSeconds 1 -StableSeconds 0 -Observe $observe } $row.Match
        }
    }

    # The table's own shape: every row uses the vocabulary, so a typo is caught
    # here rather than as a failed install.
    foreach ($rowName in $script:ServiceTable.Keys) {
        foreach ($service in $script:ServiceTable[$rowName].Keys) {
            $want = $script:ServiceTable[$rowName][$service]
            if ($null -eq $want) { continue }
            if ($want.Count -ne 2 -or $want[0] -notin $script:StartModes -or $want[1] -notin $script:States) {
                throw "row $rowName, $service`: '$($want -join ', ')' is not a start mode and a state"
            }
        }
        Pass "row shape: $rowName"
    }
    ExpectThrow 'an unknown row is refused by name' { Assert-ServiceTable -Row 'NoSuchRow' } "no service table row 'NoSuchRow'"

    # The waiting assertion against the real service control manager. EventLog is
    # auto and running on every Windows host; a fresh GUID names no service.
    $nobody = 'FastCachedSelfTest' + [guid]::NewGuid().ToString('N')
    Assert-ServiceState -Expect ([ordered]@{ EventLog = @('Auto', 'Running'); $nobody = $null }) -TimeoutSeconds 5 -StableSeconds 1
    Pass 'live: a running service stays one process, and an absent one stays absent'
    # The acquisition half of the restart check: a running service is observed
    # WITH its process, or every comparison above compares zero with zero.
    $eventLog = Get-ServiceObservation 'EventLog'
    if ($eventLog.ProcessId -le 0) { throw "live: EventLog is $($eventLog.State) and observed with process id $($eventLog.ProcessId)" }
    Pass 'live: a running service is observed with its process id'
    ExpectThrow 'live: a running service is not stopped' {
        Assert-ServiceState -Expect @{ EventLog = @('Auto', 'Stopped') } -TimeoutSeconds 1 -StableSeconds 1
    } 'EventLog is Running, expected Stopped \(waited'
    ExpectThrow 'live: a registered service is not absent' {
        Assert-ServiceState -Expect @{ EventLog = $null } -TimeoutSeconds 1 -StableSeconds 1
    } 'EventLog is still registered'
    ExpectThrow 'live: an absent service is not running' {
        Assert-ServiceState -Expect @{ $nobody = @('Auto', 'Running') } -TimeoutSeconds 1 -StableSeconds 1
    } "$nobody is not registered"

    # The package reader, over a database created here: two properties, so a
    # reader answering with the wrong row is caught, and a missing one refused.
    $scratch = Join-Path ([IO.Path]::GetTempPath()) ('msi-service-table-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    try {
        $package = Join-Path $scratch 'probe.msi'
        # [string]: Join-Path's answer arrives wrapped, and COM refuses the wrapper
        # with DISP_E_TYPEMISMATCH.
        $installer = New-Object -ComObject WindowsInstaller.Installer
        $db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @([string]$package, 3))
        foreach ($sql in @(
                'CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)',
                "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('ProductCode', '{11111111-1111-1111-1111-111111111111}')",
                "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('UpgradeCode', '{22222222-2222-2222-2222-222222222222}')",
                'CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, `Source` CHAR(72), `Target` CHAR(255) PRIMARY KEY `Action`)',
                "INSERT INTO ``CustomAction`` (``Action``, ``Type``) VALUES ('Committed', 3618)",
                "INSERT INTO ``CustomAction`` (``Action``, ``Type``) VALUES ('Deferred', 3106)")) {
            $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db, @($sql))
            $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
            $view.GetType().InvokeMember('Close', 'InvokeMethod', $null, $view, $null) | Out-Null
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
        }
        $db.GetType().InvokeMember('Commit', 'InvokeMethod', $null, $db, $null) | Out-Null
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($db)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)

        foreach ($property in @(
                @{ Name = 'ProductCode'; Value = '{11111111-1111-1111-1111-111111111111}' },
                @{ Name = 'UpgradeCode'; Value = '{22222222-2222-2222-2222-222222222222}' })) {
            $read = Get-MsiProperty $package $property.Name
            if ($read -ne $property.Value) { throw "package: $($property.Name) read '$read', expected '$($property.Value)'" }
            Pass "package: reads $($property.Name)"
        }
        ExpectThrow 'package: a missing property is refused by name' { Get-MsiProperty $package 'ProductVersion' } 'carries no ProductVersion property'

        # The custom action reader: two rows, so a reader answering with the wrong row is caught, and the
        # commit bit (0x200) ALONE separates them -- 3618 is 0xE22 and 3106 is 0xC22, both a type-34
        # in-script, no-impersonation action -- so the rows are the shapes the failed-upgrade CI step's
        # control C reads and writes. A missing action is refused by name.
        foreach ($action in @(@{ Name = 'Committed'; Type = 3618 }, @{ Name = 'Deferred'; Type = 3106 })) {
            $type = Get-MsiCustomActionType $package $action.Name
            if ($type -isnot [int] -or $type -ne $action.Type) { throw "package: custom action $($action.Name) read '$type', expected $($action.Type)" }
            Pass "package: reads the Type of custom action $($action.Name)"
        }
        ExpectThrow 'package: a missing custom action is refused by name' { Get-MsiCustomActionType $package 'NoSuchAction' } 'carries no NoSuchAction custom action'

        # The control copy (#1629): a matching UPDATE reads back, an UPDATE matching no row is refused by name.
        $copy = Join-Path $scratch 'control.msi'
        $readBack = @{ 'SELECT `Value` FROM `Property` WHERE `Property` = ''ProductCode''' = '{33333333-3333-3333-3333-333333333333}' }
        New-MsiControlCopy -Package $package -Destination $copy `
            -Statements @("UPDATE ``Property`` SET ``Value`` = '{33333333-3333-3333-3333-333333333333}' WHERE ``Property`` = 'ProductCode'") -Verify $readBack
        if ((Get-MsiProperty $copy 'ProductCode') -ne '{33333333-3333-3333-3333-333333333333}') { throw 'control copy: the patched value is not in the copy' }
        if ((Get-MsiProperty $package 'ProductCode') -ne '{11111111-1111-1111-1111-111111111111}') { throw 'control copy: the ORIGINAL was changed' }
        Pass 'control copy: a matching UPDATE reads back, and the original is untouched'
        ExpectThrow 'control copy: an UPDATE matching no row is refused by name' {
            New-MsiControlCopy -Package $package -Destination (Join-Path $scratch 'nomatch.msi') `
                -Statements @("UPDATE ``Property`` SET ``Value`` = 'x' WHERE ``Property`` = 'NoSuchProperty'") `
                -Verify @{ 'SELECT `Value` FROM `Property` WHERE `Property` = ''NoSuchProperty''' = 'x' }
        } "= 'NoSuchProperty'' read '<no row>', expected 'x'$"
        ExpectThrow 'control copy: a row present with another value is refused, naming both' {
            New-MsiControlCopy -Package $package -Destination (Join-Path $scratch 'wrong.msi') `
                -Statements @("UPDATE ``Property`` SET ``Value`` = 'abc' WHERE ``Property`` = 'ProductCode'") `
                -Verify @{ 'SELECT `Value` FROM `Property` WHERE `Property` = ''ProductCode''' = 'xyz' }
        } "= 'ProductCode'' read 'abc', expected 'xyz'$"
        ExpectThrow 'control copy: a readback differing only in case is refused' {
            New-MsiControlCopy -Package $package -Destination (Join-Path $scratch 'case.msi') `
                -Statements @("UPDATE ``Property`` SET ``Value`` = 'abc' WHERE ``Property`` = 'ProductCode'") `
                -Verify @{ 'SELECT `Value` FROM `Property` WHERE `Property` = ''ProductCode''' = 'ABC' }
        } "read 'abc', expected 'ABC'"
    } finally {
        Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
    }

    # The installed-product reader, in the one direction a host that installs
    # nothing can reach: an unused UpgradeCode answers an EMPTY ARRAY, which is
    # what the comparison in Assert-InstalledProduct needs, not $null.
    $none = Get-InstalledProductCodes ('{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}')
    if ($null -eq $none -or $none -isnot [array] -or $none.Count -ne 0) { throw "installed products: an unused UpgradeCode answered '$none'" }
    Pass 'installed products: an unused UpgradeCode answers an empty array'
    Assert-InstalledProduct ('{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}') ''
    Pass 'installed products: none expected and none installed'
    ExpectThrow 'installed products: one expected and none installed' {
        Assert-InstalledProduct ('{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}') '{33333333-3333-3333-3333-333333333333}'
    } 'expected \[\{33333333'

    # The downgrade decision: older is refused with its cause, equal and newer
    # are upgrades, and a version that does not parse is refused rather than
    # compared as text.
    foreach ($row in @(
            @{ Case = 'an older build is refused'; This = '0.3.0'; Latest = '0.4.0'; Tag = 'v0.4.0'; Match = 'this build is 0\.3\.0, older than the latest release v0\.4\.0 \(0\.4\.0\); rebase onto a tree that contains the tag' }
            @{ Case = 'an older build is refused by number, not by text'; This = '0.9.0'; Latest = '0.10.0'; Tag = 'v0.10.0'; Match = 'older than the latest release' }
            @{ Case = 'an equal build is an upgrade'; This = '0.4.0'; Latest = '0.4.0'; Tag = 'v0.4.0'; Match = $null }
            @{ Case = 'a newer build is an upgrade'; This = '0.5.0'; Latest = '0.4.0'; Tag = 'v0.4.0'; Match = $null }
            @{ Case = 'an unparseable version is refused'; This = '0.5.0'; Latest = 'garbage'; Tag = 'v9.9.9'; Match = "ProductVersion 'garbage' is not a version" })) {
        $verdict = Get-UpgradeVersionVerdict $row.This $row.Latest $row.Tag
        if ($null -eq $row.Match) {
            if ($null -ne $verdict) { throw "version: $($row.Case): expected no verdict, got '$verdict'" }
        } elseif ($null -eq $verdict -or $verdict -notmatch $row.Match) {
            throw "version: $($row.Case): expected a verdict matching '$($row.Match)', got '$verdict'"
        }
        Pass "version: $($row.Case)"
    }

    # The log assertions, over a log written here in the verbose format.
    $log = Join-Path ([IO.Path]::GetTempPath()) ('msi-service-table-' + [guid]::NewGuid().ToString('N') + '.log')
    Set-Content -Path $log -Value @(
        'MSI (s) (7C:40) [10:00:00:000]: Doing action: FastCacheNodeDeleteLeftover',
        'MSI (s) (7C:40) [10:00:00:001]: Skipping action: FastCacheNodeUninstallService (condition is false)',
        'MSI (s) (7C:40) [10:00:00:002]: Doing action: FastCacheNodeStartServiceLater')
    try {
        Assert-MsiLog -Path $log -Present (Get-MsiActionRanPattern 'FastCacheNodeDeleteLeftover') `
            -Absent (Get-MsiActionRanPattern 'FastCacheNodeUninstallService'), (Get-MsiActionRanPattern 'FastCacheNodeStartService')
        Pass 'log: a ran action is present, a skipped one and a prefix of a longer name are not'
        ExpectThrow 'log: a skipped action is not a ran one' {
            Assert-MsiLog -Path $log -Present (Get-MsiActionRanPattern 'FastCacheNodeUninstallService')
        } 'has no line matching'
        ExpectThrow 'log: a ran action is refused where it must be absent' {
            Assert-MsiLog -Path $log -Absent (Get-MsiActionRanPattern 'FastCacheNodeDeleteLeftover')
        } 'has 1 line\(s\) matching'
    } finally {
        Remove-Item -Force $log -ErrorAction SilentlyContinue
    }

    # The directory-owner verdict Assert-NodeStatePrivate reads, driven without a real
    # directory. The rest of that function reads a live ACL and is one of the module's stated
    # blind spots; this pure part is not.
    if ($null -ne (Get-DirectoryOwnerVerdict 'S-1-5-32-544')) { throw 'owner verdict: Administrators must pass' }
    Pass 'owner: Administrators owns the state directory'
    ExpectThrow 'owner: a non-administrative owner is refused' {
        if ($verdict = Get-DirectoryOwnerVerdict 'S-1-5-21-1-2-3-1001') { throw $verdict }
    } 'expected Administrators'

    # The node firewall verdict Assert-NodeFirewall reads: the exact set, in any order, and
    # WHICH names a wrong set lacks or carries.
    $four = @('a tcp/1', 'b tcp/2', 'c udp/3', 'd udp/any')
    if ($null -ne (Get-NodeFirewallVerdict @('d udp/any', 'c udp/3', 'b tcp/2', 'a tcp/1') $four)) {
        throw 'firewall verdict: the same set in another order must pass'
    }
    Pass 'firewall: the same set in another order passes'
    ExpectThrow 'firewall: the node port alone is refused, naming what it lacks' {
        if ($verdict = Get-NodeFirewallVerdict @('a tcp/1') $four) { throw $verdict }
    } 'lacks \[b tcp/2, c udp/3, d udp/any\]'
    ExpectThrow 'firewall: a rule beyond the service is refused, naming it' {
        if ($verdict = Get-NodeFirewallVerdict ($four + 'e tcp/9') $four) { throw $verdict }
    } 'carries \[e tcp/9\]'

    # The scope verdict Assert-NodeFirewallScope reads: the mask spelling Windows reports passes, a
    # rule left open to any address is refused by name, and so is a rule with another scope.
    $scoped = @(@{ Name = 'a tcp/1'; Remote = @('10.0.0.0/255.0.0.0') }, @{ Name = 'b udp/2'; Remote = @('10.0.0.0/8') })
    if ($null -ne (Get-NodeFirewallScopeVerdict $scoped '10.0.0.0/8')) { throw 'scope verdict: both spellings of the scope must pass' }
    Pass 'scope: the mask spelling Windows reports and the prefix spelling both pass'
    ExpectThrow 'scope: a rule open to any address is refused, naming it' {
        if ($verdict = Get-NodeFirewallScopeVerdict (@($scoped[0]) + @(@{ Name = 'b udp/2'; Remote = @('Any') })) '10.0.0.0/8') { throw $verdict }
    } 'b udp/2 admits \[Any\]'
    ExpectThrow 'scope: a rule with another scope is refused' {
        if ($verdict = Get-NodeFirewallScopeVerdict @(@{ Name = 'a tcp/1'; Remote = @('192.168.0.0/255.255.0.0') }) '10.0.0.0/8') { throw $verdict }
    } 'not scoped to 10\.0\.0\.0/8'

    # The registration verdict Assert-NodeRegistrationArgument reads: a whole token passes behind a
    # quoted program path with spaces, and neither its absence nor a longer value passes.
    $imagePath = '"C:\Program Files\fastcached\bin\fastcache-compile-node.exe" --service --fleet-seed=seed.example.invalid:6674'
    if ($null -ne (Get-NodeRegistrationArgumentVerdict $imagePath '--fleet-seed=seed.example.invalid:6674')) {
        throw 'registration verdict: a whole token must pass'
    }
    Pass 'registration: the argument as a whole token passes'
    ExpectThrow 'registration: a command line without it is refused, naming it' {
        if ($verdict = Get-NodeRegistrationArgumentVerdict '"C:\Program Files\x.exe" --service' '--fleet-seed=a:6674') { throw $verdict }
    } 'does not carry --fleet-seed=a:6674'
    ExpectThrow 'registration: a longer value is not the argument' {
        if ($verdict = Get-NodeRegistrationArgumentVerdict 'x.exe --fleet-seed=a:66740' '--fleet-seed=a:6674') { throw $verdict }
    } 'does not carry'

    # The diagnosis's command-line split: a quoted program path with spaces is one token, and so
    # is a value quoted after its `=`; the quotes are dropped either way.
    $split = @(Split-RegisteredCommandLine '"C:\Program Files\fastcached\bin\fastcache-compile-node.exe" --daemon --cache-dir="C:\dir with space" --x=1')
    if (($split -join '|') -ne 'C:\Program Files\fastcached\bin\fastcache-compile-node.exe|--daemon|--cache-dir=C:\dir with space|--x=1') {
        throw "split: got '$($split -join '|')'"
    }
    Pass 'split: a quoted program path and a quoted value are one token each'
    $single = @(Split-RegisteredCommandLine 'node.exe')
    if ($single.Count -ne 1 -or $single[0] -ne 'node.exe') { throw "split: a bare program gave '$($single -join '|')'" }
    Pass 'split: a bare program is one token'

    # The diagnosis never throws, so it cannot replace the failure it runs beside: a name no host
    # registers is reported as such.
    $said = Show-ServiceDiagnosis 'FastCacheSelfTestNoSuchService' 6>&1 | Out-String
    if ($said -notmatch 'not registered') { throw "diagnosis: an unregistered service said '$said'" }
    Pass 'diagnosis: an unregistered service is reported, not thrown'

    # The order verdict, over a synthetic upgrade log: the old product's uninstall, then this
    # product's registration and start, passes; each way it can go wrong is named.
    $upgradeLog = @(
        'MSI (s) (7C:40) [10:00:00:001]: Doing action: FastCacheNodeUninstallService',
        'MSI (s) (7C:40) [10:00:00:002]: Doing action: FastCacheNodeInstallService',
        'MSI (s) (7C:40) [10:00:00:003]: Doing action: FastCacheNodeStartService')
    $order = 'FastCacheNodeUninstallService', 'FastCacheNodeInstallService', 'FastCacheNodeStartService'
    if ($null -ne ($v = Get-MsiActionOrderVerdict $upgradeLog $order)) { throw "order: the sequence as it must run was refused: $v" }
    Pass 'order: the old uninstall, then the registration, then the start, passes'
    ExpectThrow 'order: the old uninstall AFTER the registration is refused' {
        if ($v = Get-MsiActionOrderVerdict @($upgradeLog[1], $upgradeLog[0], $upgradeLog[2]) $order) { throw $v }
    } 'FastCacheNodeInstallService ran \(line 1\) before FastCacheNodeUninstallService'
    ExpectThrow 'order: a registration that never ran is named' {
        if ($v = Get-MsiActionOrderVerdict @($upgradeLog[0], $upgradeLog[2]) $order) { throw $v }
    } 'FastCacheNodeInstallService never ran'
    ExpectThrow 'order: a skipped action is not a run' {
        if ($v = Get-MsiActionOrderVerdict @($upgradeLog[0], $upgradeLog[1], 'Skipping action: FastCacheNodeStartService (condition is false)') $order) { throw $v }
    } 'FastCacheNodeStartService never ran'

    # The absence verdict: 0.3.0's registration is refused, the new one passes, and a flag that
    # merely shares the prefix is not the flag.
    $old030 = '"C:\Program Files\fastcached\bin\fastcache-compile-node.exe" --daemon --service-name=FastCacheCompileNode --scheduler=127.0.0.1:6675 --advertise=127.0.0.1:6674'
    ExpectThrow 'absent: a registration still carrying --scheduler is refused' {
        if ($v = Get-NodeRegistrationLacksVerdict $old030 '--scheduler') { throw $v }
    } 'still carries --scheduler'
    if ($null -ne (Get-NodeRegistrationLacksVerdict '"C:\Program Files\x.exe" --daemon --schedulers-seen=1 --node-id=a' '--scheduler')) {
        throw 'absent: a different flag sharing the prefix was taken for it'
    }
    Pass 'absent: a registration without it passes, and a longer flag name is not it'

    # The absence verdict Assert-NodeRegistrationLacks reads: a command line with no such token passes,
    # one carrying it is refused naming the token, and a prefix inside a quoted program path is no token.
    if ($null -ne (Get-NodeRegistrationLacksVerdict $imagePath '--advertise')) {
        throw 'absence verdict: a registration with no advertise flag must pass'
    }
    Pass 'absence: a registration carrying no such token passes'
    ExpectThrow 'absence: a registration still carrying the flag is refused, naming it' {
        if ($verdict = Get-NodeRegistrationLacksVerdict "$imagePath --advertise=10.8.0.7:6674" '--advertise') { throw $verdict }
    } 'still carries --advertise=10\.8\.0\.7:6674'
    if ($null -ne (Get-NodeRegistrationLacksVerdict '"C:\--advertise dir\x.exe" --service' '--advertise')) {
        throw 'absence verdict: the quoted program path is one token, and it does not start with the prefix'
    }
    Pass 'absence: a prefix inside the quoted program path is not a token of its own'

    # The emptiness verdict Assert-FirewallGroupEmpty reads: no rule passes, one left behind is refused
    # by name.
    if ($null -ne (Get-FirewallGroupEmptyVerdict @() 'fastcached: X')) { throw 'group verdict: an empty group must pass' }
    Pass 'group: an empty group passes'
    ExpectThrow 'group: a rule left behind is refused, naming it' {
        if ($verdict = Get-FirewallGroupEmptyVerdict @('X node tcp/6674') 'fastcached: X') { throw $verdict }
    } "'fastcached: X' still holds \[X node tcp/6674\]"

    # The before-and-after verdict Assert-FirewallGroupUnchanged reads (R4-3): the same rules pass in
    # any order, a scope the rollback did not put back is refused naming both lines, and an empty
    # BEFORE is refused rather than compared.
    $rule = 'FastCached cache tcp/6674 | enabled=True Inbound Allow | TCP/6674 | remote=Any'
    $scoped = 'FastCached cache tcp/6674 | enabled=True Inbound Allow | TCP/6674 | remote=10.0.0.0/255.0.0.0'
    $other = 'FastCached admin tcp/6675 | enabled=True Inbound Allow | TCP/6675 | remote=Any'
    if ($null -ne (Get-FirewallGroupUnchangedVerdict @($rule, $other) @($other, $rule) 'fastcached: X')) {
        throw 'unchanged verdict: the same rules in another order must pass'
    }
    Pass 'unchanged: the same rules in another order pass'
    ExpectThrow 'unchanged: a scope the rollback left in place is refused, naming what was lost and what is held' {
        if ($verdict = Get-FirewallGroupUnchangedVerdict @($rule) @($scoped) 'fastcached: X') { throw $verdict }
    } 'lost \[FastCached cache tcp/6674 .*remote=Any\] and holds \[FastCached cache tcp/6674 .*remote=10\.0\.0\.0'
    ExpectThrow 'unchanged: an empty group before the transaction proves nothing, and is refused' {
        if ($verdict = Get-FirewallGroupUnchangedVerdict @() @() 'fastcached: X') { throw $verdict }
    } 'held no rule BEFORE the transaction'
    # And the snapshot LINE carries the profile and the program (round 11 review, M6): a rollback that
    # put the port and the scope back but admitted another program, or another profile, is refused.
    $ruleObject = [pscustomobject] @{ DisplayName = 'FastCached cache tcp/6674'; Enabled = 'True'; Direction = 'Inbound'; Action = 'Allow'; Profile = 'Any' }
    $portFilter = [pscustomobject] @{ Protocol = 'TCP'; LocalPort = '6674' }
    $addressFilter = [pscustomobject] @{ RemoteAddress = 'Any' }
    $programBefore = Format-FirewallRuleLine $ruleObject $portFilter $addressFilter ([pscustomobject] @{ Program = 'C:\Program Files\fastcached\bin\fastcached.exe' })
    $programAfter = Format-FirewallRuleLine $ruleObject $portFilter $addressFilter ([pscustomobject] @{ Program = 'C:\Elsewhere\fastcached.exe' })
    $privateOnly = [pscustomobject] @{ DisplayName = 'FastCached cache tcp/6674'; Enabled = 'True'; Direction = 'Inbound'; Action = 'Allow'; Profile = 'Private' }
    $profileAfter = Format-FirewallRuleLine $privateOnly $portFilter $addressFilter ([pscustomobject] @{ Program = 'C:\Program Files\fastcached\bin\fastcached.exe' })
    ExpectThrow 'unchanged: a rollback that admits another PROGRAM is refused, naming it' {
        if ($verdict = Get-FirewallGroupUnchangedVerdict @($programBefore) @($programAfter) 'fastcached: X') { throw $verdict }
    } 'holds \[.*program=C:\\Elsewhere\\fastcached\.exe\]'
    ExpectThrow 'unchanged: a rollback that changes the PROFILE is refused, naming it' {
        if ($verdict = Get-FirewallGroupUnchangedVerdict @($programBefore) @($profileAfter) 'fastcached: X') { throw $verdict }
    } 'holds \[.*profile=Private'
    # A rule the rollback DUPLICATED renders as the same line twice, and a set comparison reads that as
    # unchanged (#1629, M1): the comparison is a multiset.
    ExpectThrow 'unchanged: a rule the rollback duplicated is refused, naming the extra one' {
        if ($verdict = Get-FirewallGroupUnchangedVerdict @($rule) @($rule, $rule) 'fastcached: X') { throw $verdict }
    } 'lost \[\] and holds \[FastCached cache tcp/6674 .*remote=Any\] it did not$'

    # The firewall snapshot's error decision (#1629, M2): a group that holds no rule reads as empty, and
    # any OTHER failed read is thrown, never read as an empty group that then compares as unchanged.
    $failWith = { param([string] $id, [string] $message)
        { param($group) throw [Management.Automation.ErrorRecord]::new([Exception]::new($message), $id, 'NotSpecified', $group) }.GetNewClosure() }
    $empty = @(Get-FirewallGroupSnapshot 'fastcached: X' -Read (& $failWith $script:FirewallGroupNotFoundId 'No MSFT_NetFirewallRule objects found'))
    if ($empty.Count -ne 0) { throw "firewall snapshot: a group holding no rule answered '$($empty -join ' | ')'" }
    Pass 'firewall snapshot: a group holding no rule reads as empty'
    ExpectThrow 'firewall snapshot: any other failed read is thrown, never read as empty' {
        Get-FirewallGroupSnapshot 'fastcached: X' -Read (& $failWith 'HRESULT 0x80070005,Get-NetFirewallRule' 'Access is denied')
    } '^Access is denied$'
    # And the id is the one the REAL firewall raises: a group no host has, read live.
    $noGroup = @(Get-FirewallGroupSnapshot ('fastcached: SelfTest' + [guid]::NewGuid().ToString('N')))
    if ($noGroup.Count -ne 0) { throw "firewall snapshot: a group no host has answered '$($noGroup -join ' | ')'" }
    Pass 'firewall snapshot: live, a group no host has reads as empty rather than failing'

    # The registry walker every snapshot field of a service is read through (#1629, M3), over a scratch
    # key in HKCU: each value kind, a subkey, the data unexpanded, and one volatile row excluded for its
    # own service and no other. Then the same key absent.
    $hkcu = [Microsoft.Win32.Registry]::CurrentUser
    $scratchKey = 'Software\fastcached-selftest-' + [guid]::NewGuid().ToString('N')
    $volatileRow = 'SelfTestSvc\Sub\Volatile'
    $created = $hkcu.CreateSubKey($scratchKey)
    try {
        $created.SetValue('Text', 'plain', [Microsoft.Win32.RegistryValueKind]::String)
        $created.SetValue('Path', '%SystemRoot%\x', [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $created.SetValue('Count', 2, [Microsoft.Win32.RegistryValueKind]::DWord)
        $created.SetValue('List', [string[]] @('a', 'b c'), [Microsoft.Win32.RegistryValueKind]::MultiString)
        $created.SetValue('Bytes', [byte[]] @(0x01, 0xab), [Microsoft.Win32.RegistryValueKind]::Binary)
        $sub = $created.CreateSubKey('Sub')
        try {
            $sub.SetValue('Inner', 'deep', [Microsoft.Win32.RegistryValueKind]::String)
            $sub.SetValue('Volatile', 'moves', [Microsoft.Win32.RegistryValueKind]::String)
        } finally { $sub.Dispose() }
        $script:VolatileServiceValues[$volatileRow] = 'self-test row: drives the exclusion branch'
        $expected = @('Bytes Binary 01ab', 'Count DWord 2', 'List MultiString "a","b c"', 'Path ExpandString %SystemRoot%\x',
            'Sub\Inner String deep', 'Text String plain') | Sort-Object
        $lines = Get-RegistryTreeLines $scratchKey 'SelfTestSvc' $hkcu
        if (($lines -join ' | ') -cne ($expected -join ' | ')) { throw "registry tree: read [$($lines -join ' | ')], expected [$($expected -join ' | ')]" }
        Pass 'registry tree: every kind, a subkey, unexpanded, and its own volatile row excluded'
        $unexcluded = Get-RegistryTreeLines $scratchKey 'OtherSvc' $hkcu
        if ($unexcluded -cnotcontains 'Sub\Volatile String moves') { throw "registry tree: another service's volatile row excluded a value: [$($unexcluded -join ' | ')]" }
        Pass 'registry tree: a volatile row excludes nothing for another service'
    } finally {
        $script:VolatileServiceValues.Remove($volatileRow)
        $created.Dispose()
        $hkcu.DeleteSubKeyTree($scratchKey, $false)
    }
    $gone = Get-RegistryTreeLines $scratchKey 'SelfTestSvc' $hkcu
    if ($gone -isnot [array] -or ($gone -join ' | ') -cne '<absent>') { throw "registry tree: an absent key read [$($gone -join ' | ')]" }
    Pass 'registry tree: an absent key reads as the one line <absent>'

    # What failed, read whatever shape Windows Installer gave it: an action that could not START names
    # no CustomAction and no code (round 5), one that ran and failed does, and a clean log says nothing.
    $startFailure = @('Action start 5:34:04: InstallFiles.', 'noise', 'noise', 'noise',
        'MSI (s) (88:AC) [05:34:04:780]: Product: fastcached -- Error 1721. There is a problem with this Windows Installer package. Action: FastCacheAwaitServiceExit, location: C:\Program Files\fastcached\, command: ...',
        'after')
    $found = @(Get-MsiFailureLines $startFailure)
    if ($found.Count -ne 4 -or $found[-1] -notmatch 'Error 1721.*FastCacheAwaitServiceExit' -or $found -contains 'after') {
        throw "failure lines: an action that could not start was not reported with its context: $($found -join ' | ')"
    }
    Pass 'failure lines: an action that could not start is reported, with the lines before it'
    $ranAndFailed = @('CustomAction FastCacheNodeStartService returned actual error code 2 (note this may not be 100% accurate)')
    if (@(Get-MsiFailureLines $ranAndFailed).Count -ne 1) { throw 'failure lines: an action that ran and failed was not reported' }
    Pass 'failure lines: an action that ran and failed is reported'
    $ignoredThenFailed = @('CustomAction FastCacheClearRollbackState returned actual error code 1 but will be translated to success due to continue marking',
        'one', 'two', 'three', 'four', $ranAndFailed[0])
    $found = @(Get-MsiFailureLines $ignoredThenFailed)
    if ($found.Count -ne 4 -or $found[0] -ne 'two' -or $found[-1] -ne $ranAndFailed[0]) {
        throw "failure lines: an ignored action's non-zero code was reported as a failure: $($found -join ' | ')"
    }
    Pass 'failure lines: an action translated to success is not a failure, and the real one is'
    if (@(Get-MsiFailureLines @('Action ended 5:34:04: FastCacheNodeStartService. Return value 1.', 'Installation success or error status: 0.')).Count -ne 0) {
        throw 'failure lines: a clean log reported a failure'
    }
    Pass 'failure lines: a clean log reports nothing'

    # The RESTART MANAGER lines: every one up to the cap, and the cut counted. The first four are the
    # four PR 1634's CI printed; the nested removal's session after them is what a cap of four cut.
    $restartLog = @(
        'MSI (s) (D8:0C) [13:31:45:209]: RESTART MANAGER: Disabled by MSIRESTARTMANAGERCONTROL property; Windows Installer will use the built-in FilesInUse functionality.',
        'MSI (s) (D8:AC) [13:31:46:577]: RESTART MANAGER: Session opened.', 'Property(S): noise',
        'MSI (s) (D8:AC) [13:31:46:717]: RESTART MANAGER: Will attempt to shut down and restart applications in no UI modes.',
        'MSI (c) (80:EC) [13:31:46:720]: RESTART MANAGER: Session opened.',
        'MSI (s) (D8:AC) [13:31:47:001]: RESTART MANAGER: Successfully shut down all applications in the service''s session that held files in use.',
        'MSI (s) (D8:AC) [13:32:20:100]: RESTART MANAGER: Restarted the applications.')
    $found = Get-MsiRestartManagerLines $restartLog
    if ($found.Lines.Count -ne 6 -or $found.Omitted -ne 0 -or $found.Lines[-1] -notmatch 'Restarted the applications') {
        throw "restart manager lines: read $($found.Lines.Count), $($found.Omitted) omitted: $($found.Lines -join ' | ')"
    }
    Pass 'restart manager lines: every line is returned, the ones past the fourth included'
    $found = Get-MsiRestartManagerLines $restartLog -Cap 4
    if ($found.Lines.Count -ne 4 -or $found.Omitted -ne 2) { throw "restart manager lines: a cap of 4 returned $($found.Lines.Count), $($found.Omitted) omitted" }
    Pass 'restart manager lines: a cap returns that many and counts what it cut'

    # The log's clock: Windows Installer's own stamp and an action's, carried forward to a line with
    # none, and nothing before the first stamp.
    $clock = @(Get-MsiLogTimeOfDay @('=== Verbose logging started ===', 'MSI (s) (D8:AC) [13:31:46:577]: Doing action: X', 'Property(S): P = 1',
            'Action start 9:05:07: InstallValidate.'))
    if ($clock.Count -ne 4 -or $null -ne $clock[0] -or $clock[1] -ne [TimeSpan]::new(0, 13, 31, 46, 577) -or $clock[2] -ne $clock[1] -or
        $clock[3] -ne [TimeSpan]::new(9, 5, 7)) {
        throw "log clock: read [$($clock -join ', ')]"
    }
    Pass 'log clock: both stamps are read, carried to unstamped lines, and absent before the first'

    # The lines around a finding: an action, a service operation, a Restart Manager line and a nested
    # product's start inside the window; noise inside it, and an action outside it, are not; two
    # findings' windows merge; and a window across midnight still matches.
    $around = @(
        'MSI (s) (D8:AC) [13:31:40:000]: Doing action: InstallInitialize',
        'MSI (s) (D8:AC) [13:32:00:000]: Running product ''{FDCDAB73-40AF-4EDA-9340-7C33986C3207}'' with elevated privileges: Product is assigned.',
        'Action start 13:32:01: InstallValidate.', 'Property(S): noise',
        'MSI (s) (D8:AC) [13:32:19:900]: Executing op: ServiceControl(,Name=FastCached,Action=1,Wait=0,)',
        'MSI (s) (D8:AC) [13:32:20:100]: RESTART MANAGER: Restarted the applications.',
        'MSI (s) (D8:AC) [13:32:21:000]: Note: 1: 2205 2:  3: Error',
        'MSI (s) (D8:AC) [13:32:40:000]: Product: fastcached -- Installation completed successfully.',
        'MSI (s) (D8:AC) [00:00:02:000]: Doing action: AfterMidnight')
    $got = Get-MsiLinesAround -Lines $around -Times @([TimeSpan]::new(0, 13, 32, 20, 306), [TimeSpan]::new(0, 13, 32, 24, 585))
    if ($got.Omitted -ne 0 -or ($got.Lines -join '|') -notmatch '^MSI .*Running product .*\|Action start 13:32:01: InstallValidate\.\|.*ServiceControl\(,Name=FastCached.*\|.*RESTART MANAGER: Restarted the applications\.$') {
        throw "lines around: kept [$($got.Lines -join ' | ')]"
    }
    Pass 'lines around: the actions, service operations and Restart Manager lines in the window, once, in log order'
    $got = Get-MsiLinesAround -Lines $around -Times @([TimeSpan]::new(23, 59, 58)) -BeforeSeconds 1 -AfterSeconds 5
    if (($got.Lines -join '|') -notmatch '^MSI .*Doing action: AfterMidnight$') { throw "lines around: across midnight kept [$($got.Lines -join ' | ')]" }
    Pass 'lines around: a window across midnight matches the next day''s first seconds'
    $got = Get-MsiLinesAround -Lines $around -Times @([TimeSpan]::new(0, 13, 32, 20)) -Cap 2
    if ($got.Lines.Count -ne 2 -or $got.Omitted -ne 2) { throw "lines around: a cap of 2 kept $($got.Lines.Count), $($got.Omitted) omitted" }
    Pass 'lines around: a cap keeps that many and counts what it cut'

    # How the node's service reaches its state, over REAL access lists and under this module's strict
    # mode, which is where the inline version threw (round 6, C1): ONE entry naming the service is
    # exactly the list the install produces, and `.Count` on that lone pipeline result was an
    # error. The current account stands in for the service; its key inherits the directory's entry.
    $me = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $fsRights = [Security.AccessControl.FileSystemRights]
    $inherit = [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'
    $newStateDirectory = {
        param([object[]] $Entries)
        $directory = Join-Path ([IO.Path]::GetTempPath()) ("msi-selftest-state-" + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $directory | Out-Null
        $acl = New-Object Security.AccessControl.DirectorySecurity
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($entry in $Entries) { $acl.AddAccessRule($entry) }
        Set-Acl -LiteralPath $directory -AclObject $acl
        # Nothing else: the account created the directory, so as its owner it keeps the
        # READ_CONTROL and WRITE_DAC that let it set this list, and each case's entries let it
        # delete what it made.
        Set-Content -LiteralPath (Join-Path $directory 'node-key') -Value 'seed'
        return $directory
    }
    $modify = New-Object Security.AccessControl.FileSystemAccessRule($me, [Security.AccessControl.FileSystemRights]0x1301bf, $inherit, 'None', 'Allow')
    $reachCases = @(
        @{ Name = 'one inherited Modify entry, as installed'; Entries = @($modify); Want = $null },
        @{ Name = 'an entry that may rewrite the list'; Entries = @((New-Object Security.AccessControl.FileSystemAccessRule($me, [Security.AccessControl.FileSystemRights]0x1701bf, $inherit, 'None', 'Allow')));
           Want = 'may rewrite' },
        @{ Name = 'an INHERIT-ONLY entry that may rewrite, beside the Modify one'; Entries = @($modify,
               (New-Object Security.AccessControl.FileSystemAccessRule($me, $fsRights::ChangePermissions, $inherit, 'InheritOnly', 'Allow'))); Want = $null }
    )
    foreach ($case in $reachCases) {
        $directory = & $newStateDirectory $case.Entries
        try {
            $verdict = Get-NodeServiceAccessVerdict $directory (Join-Path $directory 'node-key') $me.Value
            if ($null -eq $case.Want -and $null -ne $verdict) { throw "service access: '$($case.Name)' was refused: $verdict" }
            if ($null -ne $case.Want -and ($null -eq $verdict -or $verdict -notmatch $case.Want)) {
                throw "service access: '$($case.Name)' answered '$verdict', expected '$($case.Want)'"
            }
            Pass "service access: $($case.Name) is $(if ($null -eq $case.Want) { 'accepted' } else { 'refused by name' })"
        } finally {
            Remove-Item -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # The installation comparison, over synthetic snapshots (#1629): every outcome, and WHICH field.
    $base = [ordered]@{ 'service FastCached' = @('ImagePath ExpandString "C:\r\fastcached.exe" --daemon', 'Start DWord 2'); 'state FastCached' = @('Running') }
    $cmp = @(
        @{ Case = 'equal snapshots'; After = $base; Match = $null }
        @{ Case = 'one changed value'; After = [ordered]@{ 'service FastCached' = @('ImagePath ExpandString "C:\r\fastcached.exe" --daemon', 'Start DWord 3'); 'state FastCached' = @('Running') }; Match = '^service FastCached: lost \[Start DWord 2\]; gained \[Start DWord 3\]$' }
        @{ Case = 'a changed state'; After = [ordered]@{ 'service FastCached' = $base['service FastCached']; 'state FastCached' = @('Stopped') }; Match = '^state FastCached: lost \[Running\]; gained \[Stopped\]$' }
        @{ Case = 'a field only one side has'; After = [ordered]@{ 'service FastCached' = $base['service FastCached'] }; Match = '^state FastCached: lost \[Running\]; gained \[\]$' }
        @{ Case = 'a key that vanished'; After = [ordered]@{ 'service FastCached' = @('<absent>'); 'state FastCached' = @('Running') }; Match = '^service FastCached: lost' }
        @{ Case = 'a line held twice where it was held once'; After = [ordered]@{ 'service FastCached' = @($base['service FastCached']) + @('Start DWord 2'); 'state FastCached' = @('Running') }; Match = '^service FastCached: lost \[\]; gained \[Start DWord 2\]$' }
    )
    foreach ($row in $cmp) {
        $diff = @(Compare-InstallationSnapshot -Before $base -After $row.After)
        if ($null -eq $row.Match) { if ($diff.Count -ne 0) { throw "compare: $($row.Case): expected none, got [$($diff -join '; ')]" } }
        elseif ($diff.Count -ne 1 -or $diff[0] -notmatch $row.Match) { throw "compare: $($row.Case): expected one difference matching '$($row.Match)', got [$($diff -join '; ')]" }
        Pass "compare: $($row.Case)"
    }

    # Compare refuses an instrument failure BY FIELD, and sees an empty field only one side has.
    ExpectThrow 'compare: a nested array element is refused, naming the field' {
        Compare-InstallationSnapshot -Before ([ordered]@{ products = , @(, @('{A}')) }) -After ([ordered]@{ products = @('{A}') })
    } "field 'products' holds a non-string element \(Object\[\]\)"
    $oneSided = @(Compare-InstallationSnapshot -Before ([ordered]@{ a = @('x'); fw = @() }) -After ([ordered]@{ a = @('x') }))
    if ($oneSided.Count -ne 1 -or $oneSided[0] -notmatch '^fw: lost \[\]; gained \[\] <field absent after>$') { throw "compare: an empty one-sided field: got [$($oneSided -join '; ')]" }
    Pass 'compare: an empty field only one side has is a difference'

    # The settled state, through the observation seam: Pending then Running holds; a restart does not.
    $script:scripted = $null
    $scriptedObserve = { param($name) $next = $script:scripted[0]; if ($script:scripted.Count -gt 1) { $script:scripted = $script:scripted[1..($script:scripted.Count - 1)] }; $next }
    $running = { param($pid_) [pscustomobject]@{ StartMode = 'Auto'; State = 'Running'; ProcessId = $pid_ } }
    $script:scripted = @([pscustomobject]@{ StartMode = 'Auto'; State = 'Start Pending'; ProcessId = 0 }, (& $running 10), (& $running 10))
    $settled = Get-SettledServiceState -Name 'X' -StableSeconds 0 -Observe $scriptedObserve
    if ($settled -cne 'Running') { throw "settled: Pending then steady Running read '$settled'" }
    Pass 'settled: Pending then Running is a steady Running'
    $script:scripted = @((& $running 10), (& $running 11))
    $settled = Get-SettledServiceState -Name 'X' -StableSeconds 0 -Observe $scriptedObserve
    if ($settled -notmatch '^Running; unstable: X restarted within 0 s \(process 10 became 11\)') { throw "settled: a restart read '$settled'" }
    Pass 'settled: two process ids read as unstable'

    # What a transaction did to the services (PR 1634), over synthetic records. The first is the
    # upgrade from 0.3.0 that CI ran: FastCached Auto and running as 9268 before it, stopped one second
    # in, started again at 12:12:47 after the table had made it manual, and terminated three times
    # before msiexec returned. Every other row is one witness alone, or one thing that must NOT count.
    $t0 = [DateTime]::new(2026, 10, 6, 12, 12, 14, [DateTimeKind]::Utc)
    $termination = { param([int] $id, [int] $second, [string] $service)
        [pscustomobject]@{ Id = $id; Time = $t0.AddSeconds($second); Service = $service; State = ''; Message = "The $service service terminated unexpectedly. It has done this 1 time(s)." } }
    $entered = { param([int] $second, [string] $service, [string] $state)
        [pscustomobject]@{ Id = 7036; Time = $t0.AddSeconds($second); Service = $service; State = $state; Message = "The $service service entered the $($state.ToLower()) state." } }
    $sighting = { param([string] $service, [int] $processId, [int] $second, [string] $state = 'Running')
        [pscustomobject]@{ Time = $t0.AddSeconds($second); Service = $service; State = $state; ProcessId = $processId } }
    $upgradeFrom030 = @{
        Events = @((& $termination 7031 34 'FastCached'), (& $termination 7031 36 'FastCached'), (& $termination 7031 38 'FastCached'),
            (& $entered 33 'FastCached' 'Running'), (& $entered 30 'FastCacheCompileNode' 'Running'))
        Observed = @((& $sighting 'FastCached' 9268 0 'Stop Pending'), (& $sighting 'FastCacheCompileNode' 8392 30), (& $sighting 'FastCached' 5120 33))
        Baseline = @{ FastCached = 9268; FastCacheCompileNode = 0 }
    }
    $none = @{ Events = @(); Observed = @(); Baseline = @{} }
    $transactionVerdicts = @(
        @{ Case = 'the upgrade from 0.3.0 that CI ran: three terminations and a start seen by both start witnesses'; Input = $upgradeFrom030; NotRunning = @('FastCached')
           Match = @("^Termination: FastCached terminated unexpectedly during 'T': event 7031 at 2026-10-06T12:12:48\.000Z: The FastCached service terminated unexpectedly",
                     '^Termination: .*event 7031 at 2026-10-06T12:12:50\.000Z', '^Termination: .*event 7031 at 2026-10-06T12:12:52\.000Z',
                     "^Start: FastCached, which the expectation leaves not running, was started during 'T': event 7036 at 2026-10-06T12:12:47\.000Z: The FastCached service entered the running state\.$",
                     "^Start: FastCached, which the expectation leaves not running, was started during 'T': process 5120 seen Running at 2026-10-06T12:12:47\.000Z, 1 time\(s\); it ran as process 9268 when the transaction began$") }
        @{ Case = 'a termination alone, with no start seen'; NotRunning = @()
           Input = @{ Events = @(& $termination 7034 20 'FastCacheCompileNode'); Observed = @(); Baseline = @{} }
           Match = @("^Termination: FastCacheCompileNode terminated unexpectedly during 'T': event 7034 at 2026-10-06T12:12:34\.000Z") }
        @{ Case = 'a start seen by the watch alone, of a service that was not running'; NotRunning = @('FastCached')
           Input = @{ Events = @(); Observed = @(& $sighting 'FastCached' 5120 33); Baseline = @{ FastCached = 0 } }
           Match = @('^Start: .*process 5120 seen Running at 2026-10-06T12:12:47\.000Z, 1 time\(s\); it was not running when the transaction began$') }
        @{ Case = 'a start seen by 7036 alone, between two observations of the watch'; NotRunning = @('FastCached')
           Input = @{ Events = @((& $entered 33 'FastCached' 'Running'), (& $entered 34 'FastCached' 'Stopped')); Observed = @(); Baseline = @{ FastCached = 0 } }
           Match = @('^Start: FastCached, .*event 7036 at 2026-10-06T12:12:47\.000Z') }
        @{ Case = 'a stop (7036 stopped) of a service left not running is not a start'; NotRunning = @('FastCached')
           Input = @{ Events = @(& $entered 1 'FastCached' 'Stopped'); Observed = @(); Baseline = @{ FastCached = 9268 } }
           Match = @() }
        @{ Case = 'one stray process seen twice is one finding'; NotRunning = @('FastCached')
           Input = @{ Events = @(); Observed = @((& $sighting 'FastCached' 5120 33), (& $sighting 'FastCached' 5120 34)); Baseline = @{ FastCached = 0 } }
           Match = @('process 5120 seen Running at 2026-10-06T12:12:47\.000Z, 2 time\(s\)') }
        @{ Case = 'the process a service ran under when it began is not a start'; NotRunning = @('FastCached')
           Input = @{ Events = @(); Observed = @((& $sighting 'FastCached' 9268 0), (& $sighting 'FastCached' 9268 1 'Stop Pending')); Baseline = @{ FastCached = 9268 } }
           Match = @() }
        @{ Case = 'a service the expectation leaves running may be started'; NotRunning = @('FastCached')
           Input = @{ Events = @(& $entered 30 'FastCacheCompileNode' 'Running'); Observed = @(& $sighting 'FastCacheCompileNode' 8392 30); Baseline = @{ FastCacheCompileNode = 0 } }
           Match = @() }
        @{ Case = 'a termination before the transaction began is not its'; NotRunning = @()
           Input = @{ Events = @(& $termination 7031 -5 'FastCached'); Observed = @(); Baseline = @{} }
           Match = @() }
        @{ Case = "another service's termination is not this package's"; NotRunning = @()
           Input = @{ Events = @(& $termination 7034 5 'sshd'); Observed = @(); Baseline = @{} }
           Match = @() }
        @{ Case = 'a termination the expectation allows is not a finding, and the other service''s still is'; NotRunning = @(); MayTerminate = @('FastCacheCompileNode')
           Input = @{ Events = @((& $termination 7034 20 'FastCacheCompileNode'), (& $termination 7031 21 'FastCached')); Observed = @(); Baseline = @{} }
           Match = @("^Termination: FastCached terminated unexpectedly during 'T': event 7031 at 2026-10-06T12:12:35\.000Z") }
    )
    foreach ($row in $transactionVerdicts) {
        $may = if ($row.ContainsKey('MayTerminate')) { $row.MayTerminate } else { @() }
        $findings = @(Get-TransactionServiceVerdict -What 'T' -StartedUtc $t0 -Events $row.Input.Events -Observed $row.Input.Observed `
                -Baseline $row.Input.Baseline -NotRunning $row.NotRunning -MayTerminate $may | ForEach-Object { "$($_.Kind): $($_.Text)" })
        if ($findings.Count -ne $row.Match.Count) { throw "transaction: $($row.Case): expected $($row.Match.Count) finding(s), got [$($findings -join '; ')]" }
        foreach ($at in @(0..($findings.Count - 1) | Where-Object { $findings.Count -gt 0 })) {
            if ($findings[$at] -notmatch $row.Match[$at]) { throw "transaction: $($row.Case): finding $at '$($findings[$at])' does not match '$($row.Match[$at])'" }
        }
        Pass "transaction: $($row.Case)"
    }

    # What each row makes an expectation of: the services it leaves NOT RUNNING (stopped, or not
    # registered) are watched for a start, and the node's start is the 7036 control where it runs.
    $wantNotRunning = @{
        Release030WithNode = 'FastCacheCompileNode'; NodeSelected = 'FastCached'; NodeAlone = 'FastCached'
        DaemonAlone = 'FastCacheCompileNode'; DaemonAloneUnstarted = 'FastCacheCompileNode,FastCached'; NothingInstalled = 'FastCacheCompileNode,FastCached'
    }
    foreach ($rowName in $script:ServiceTable.Keys) {
        $expectation = Get-TransactionExpectation -Leaves $rowName
        $notRunning = @($expectation.NotRunning | Sort-Object) -join ','
        $want = @($wantNotRunning[$rowName] -split ',' | Sort-Object) -join ','
        $nodeStarts = $rowName -in 'NodeSelected', 'NodeAlone'
        if ($notRunning -cne $want -or $expectation.NodeStarts -ne $nodeStarts -or @($expectation.MayTerminate).Count -ne 0 -or $expectation.Name -cne "row $rowName") {
            throw "expectation: row $rowName leaves [$notRunning] not running, node starts $($expectation.NodeStarts); expected [$want], $nodeStarts"
        }
    }
    Pass 'expectation: each row watches what it leaves not running, and the node start is the control only where the node runs'
    foreach ($name in $script:TransactionExpectations.Keys) {
        $expectation = Get-TransactionExpectation -Expect $name
        if (@($expectation.NotRunning).Count -ne 0 -or $expectation.NodeStarts -or -not $expectation.Reason -or $expectation.Name -cne "expectation $name") {
            throw "expectation: $name read as $($expectation | ConvertTo-Json -Compress)"
        }
    }
    if ((@((Get-TransactionExpectation -Expect NodeCannotBind).MayTerminate) -join ',') -cne 'FastCacheCompileNode' -or @((Get-TransactionExpectation -Expect RollsBack).MayTerminate).Count -ne 0) {
        throw 'expectation: only NodeCannotBind allows a termination, and only the node''s'
    }
    Pass 'expectation: each named expectation carries its reason, judges no start, and only NodeCannotBind allows a termination'
    ExpectThrow 'expectation: naming neither is refused' { Get-TransactionExpectation } 'judges every transaction: name the service table row it -Leaves'
    ExpectThrow 'expectation: naming both is refused' { Get-TransactionExpectation -Leaves NodeSelected -Expect RollsBack } 'judges every transaction'
    ExpectThrow 'expectation: an unknown row is refused by name' { Get-TransactionExpectation -Leaves NoSuchRow } "no service table row 'NoSuchRow'"
    ExpectThrow 'expectation: an unknown expectation is refused by name' { Get-TransactionExpectation -Expect NoSuchExpectation } "no transaction expectation 'NoSuchExpectation'"
    # And Invoke-Msiexec itself: a call naming neither is refused BEFORE anything runs. Its first
    # statement resolves the expectation; and the start goes through the seam, so a neutered refusal
    # would start a harmless process here, never msiexec.
    ExpectThrow 'expectation: Invoke-Msiexec naming neither refuses before running anything' {
        Invoke-Msiexec -Operation '/i' -Package (Join-Path ([IO.Path]::GetTempPath()) "no-such-$([guid]::NewGuid().ToString('N')).msi") -Log 'unused.log' -What 'W' `
            -Start { param($arguments) Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -NoNewWindow } 6>$null
    } 'judges every transaction'

    # The 7036 witness's positive control: the node's own start, in every state.
    foreach ($row in @(
            @{ Case = 'the node''s start logged'; Events = @(& $entered 30 'FastCacheCompileNode' 'Running'); NodeStarts = $true; Want = 'live' }
            @{ Case = 'the node started and nothing logged it'; Events = @(& $entered 30 'FastCached' 'Running'); NodeStarts = $true; Want = 'absent' }
            @{ Case = 'a node start BEFORE the transaction is not its control'; Events = @(& $entered -3 'FastCacheCompileNode' 'Running'); NodeStarts = $true; Want = 'absent' }
            @{ Case = 'no node start in the transaction'; Events = @(& $entered 30 'FastCacheCompileNode' 'Running'); NodeStarts = $false; Want = 'no control' })) {
        $state = Get-Witness7036State -Events $row.Events -StartedUtc $t0 -NodeStarts $row.NodeStarts
        if ($state -cne $row.Want) { throw "7036 control: $($row.Case) read '$state', expected '$($row.Want)'" }
        Pass "7036 control: $($row.Case) is '$($row.Want)'"
    }

    # The event reader's parse, over every shape it reads: the binary data carries the SERVICE name
    # (this one copied from a real 7031 event), a 7045 a ServiceName item, param1 the display name, and
    # a 7036 its state in param2.
    $eventXml = { param([int] $id, [string] $stamp, [hashtable] $items, [string] $binaryHex)
        $binary = if ($binaryHex) { "<Binary>$binaryHex</Binary>" } else { '' }
        $data = @(foreach ($key in @($items.Keys | Sort-Object)) { "<Data Name='$key'>$($items[$key])</Data>" }) -join ''
        "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='Service Control Manager'/><EventID Qualifiers='49152'>$id</EventID><TimeCreated SystemTime='$stamp'/></System><EventData>$data$binary</EventData></Event>" }
    $hexOf = { param([string] $text) (([Text.Encoding]::Unicode.GetBytes("$text$([char] 0)")) | ForEach-Object { $_.ToString('X2') }) -join '' }
    $realBinary = '46006100730074004300610063006800650043006F006D00700069006C0065004E006F00640065000000'
    $parses = @(
        @{ Case = 'the service name is read from the binary data, the time in UTC'
           Xml = (& $eventXml 7031 '2026-10-06T12:12:48.1234567Z' @{ param1 = 'fastcache-compile-node'; param2 = '1' } $realBinary); Message = "The node`r`n terminated."
           Id = 7031; Service = 'FastCacheCompileNode'; State = ''; Text = 'The node terminated.'; Time = [DateTime]::new(2026, 10, 6, 12, 12, 48, [DateTimeKind]::Utc).AddTicks(1234567) }
        @{ Case = "with no binary data, this package's display name reads as its service name"
           Xml = (& $eventXml 7034 '2026-10-06T12:12:48Z' @{ param1 = 'fastcached'; param2 = '1' } ''); Message = 'm'
           Id = 7034; Service = 'FastCached'; State = ''; Text = 'm'; Time = [DateTime]::new(2026, 10, 6, 12, 12, 48, [DateTimeKind]::Utc) }
        @{ Case = 'another service keeps its own name'
           Xml = (& $eventXml 7034 '2026-10-06T12:12:48Z' @{ param1 = 'OpenSSH SSH Server'; param2 = '1' } (& $hexOf 'sshd')); Message = 'm'
           Id = 7034; Service = 'sshd'; State = ''; Text = 'm'; Time = [DateTime]::new(2026, 10, 6, 12, 12, 48, [DateTimeKind]::Utc) }
        @{ Case = 'a 7045 names its service in a ServiceName item, with no param1 and no binary data'
           Xml = (& $eventXml 7045 '2026-10-01T08:00:00Z' @{ ServiceName = 'cowork-svc'; ImagePath = 'C:\x.exe'; ServiceType = 'user mode service'; StartType = 'auto start'; AccountName = 'LocalSystem' } ''); Message = 'A service was installed in the system.'
           Id = 7045; Service = 'cowork-svc'; State = ''; Text = 'A service was installed in the system.'; Time = [DateTime]::new(2026, 10, 1, 8, 0, 0, [DateTimeKind]::Utc) }
        @{ Case = 'a 7036 reads its state from param2 and its service from the display name'
           Xml = (& $eventXml 7036 '2026-10-06T12:13:23Z' @{ param1 = 'FastCached'; param2 = 'running' } ''); Message = 'The FastCached service entered the running state.'
           Id = 7036; Service = 'FastCached'; State = 'Running'; Text = 'The FastCached service entered the running state.'; Time = [DateTime]::new(2026, 10, 6, 12, 13, 23, [DateTimeKind]::Utc) }
        @{ Case = 'a 7036 whose binary data reads <name>/<code> and whose param2 says nothing'
           Xml = (& $eventXml 7036 '2026-10-06T12:13:24Z' @{ param1 = 'fastcache-compile-node'; param2 = '' } (& $hexOf 'FastCacheCompileNode/1')); Message = 'm'
           Id = 7036; Service = 'FastCacheCompileNode'; State = 'Stopped'; Text = 'm'; Time = [DateTime]::new(2026, 10, 6, 12, 13, 24, [DateTimeKind]::Utc) }
    )
    foreach ($row in $parses) {
        $parsed = ConvertTo-ServiceControlEvent $row.Xml $row.Message
        if ($parsed.Id -ne $row.Id -or $parsed.Service -cne $row.Service -or $parsed.State -cne $row.State -or $parsed.Message -cne $row.Text -or
            $parsed.Time -ne $row.Time -or $parsed.Time.Kind -ne 'Utc') {
            throw "event: $($row.Case): parsed as $($parsed | ConvertTo-Json -Compress)"
        }
        Pass "event: $($row.Case)"
    }

    # The reader against the real System log: nothing since tomorrow is an empty answer, not an error,
    # and the positive control that keeps that empty answer honest: the same filter, over ids a host
    # running for any time has some of, finds one. It asserts the FILTER only -- what the parse makes
    # of whichever event is newest is the cases above, so an install (7045) arriving last is no red.
    $nothing = @(Get-ServiceControlEvents -SinceUtc ([DateTime]::UtcNow.AddDays(1)))
    if ($nothing.Count -ne 0) { throw "live events: $($nothing.Count) event(s) since tomorrow" }
    Pass 'live events: no event since the future is an empty answer'
    $common = @(7000, 7009, 7011, 7023, 7024, 7026, 7031, 7034, 7036, 7040, 7043, 7045)
    $some = @(Get-ServiceControlEvents -SinceUtc ([DateTime]::UtcNow.AddDays(-365)) -Ids $common -MaxEvents 1)
    if ($some.Count -ne 1 -or $some[0].Id -notin $common -or $some[0].Time.Kind -ne 'Utc') {
        throw "live events: the service control manager filter found $($some.Count) event(s) of ids $($common -join ', ') in a year, so an empty answer from it cannot be believed"
    }
    Pass "live events: the filter finds a service control manager event ($($some[0].Id))"

    # The judgement WIRED: the recorded transaction, its expectation, the event seam, and the message
    # per finding KIND.
    $record = { param([string] $leaves, [string] $expect, [int] $polls, [int] $failed, $observed)
        [pscustomobject]@{ What = 'the upgrade from v0.3.0'; Log = ''; StartedUtc = $t0; Baseline = $upgradeFrom030.Baseline
            Expectation = (Get-TransactionExpectation -Leaves $leaves -Expect $expect); Polls = $polls; FailedPolls = $failed
            FirstFailure = $(if ($failed) { 'FastCached: Invalid class' } else { '' }); Observed = $observed; Extended = $false } }
    ExpectThrow 'judgement: a watch that never looked is refused, naming its failed rounds' {
        Invoke-TransactionJudgement -Transaction (& $record 'NodeSelected' '' 0 3 @()) -ReadEvents { @() }
    } "were never observed .*\(3 observation round\(s\) failed, first: FastCached: Invalid class\)"
    $read = $upgradeFrom030.Events
    ExpectThrow 'judgement: the upgrade from 0.3.0 is refused with each finding and both kinds'' remedies' {
        Invoke-TransactionJudgement -Transaction (& $record 'NodeSelected' '' 120 0 $upgradeFrom030.Observed) `
            -ReadEvents { param($since) if ($since -ne $t0) { throw "read from $since" }; $read } 6>$null
    } "disturbed a service the table owns \(row NodeSelected\)(.|\n)*event 7031 at 2026-10-06T12:12:48(.|\n)*process 5120(.|\n)*a TERMINATION is the service failing(.|\n)*Restart Manager \(PR 1634\)"
    try {
        Invoke-TransactionJudgement -Transaction (& $record 'NodeSelected' '' 5 0 @()) -ReadEvents { @(& $termination 7034 20 'FastCacheCompileNode') } 6>$null
        throw 'judgement: a termination alone was not refused'
    } catch {
        if ($_.Exception.Message -notmatch 'a TERMINATION is the service failing' -or $_.Exception.Message -match 'Restart Manager') {
            throw "judgement: a termination alone was refused as '$($_.Exception.Message)'"
        }
    }
    Pass 'judgement: a termination alone names no Restart Manager cause'

    # A refusal SHOWS the transaction's log: what Show-MsiLog prints reaches the host even from inside
    # a judgement whose output its caller captures and whose throw discards that output (PR 1634's CI
    # printed three empty sections that way), and every RESTART MANAGER line and the actions around
    # each finding follow it. The log is UTF-16, and its stamps are the findings' instants on this
    # host's local clock, as Windows Installer writes them.
    $stamp = { param([int] $second) $t0.AddSeconds($second).ToLocalTime().ToString('HH:mm:ss:fff', [Globalization.CultureInfo]::InvariantCulture) }
    $judgedLog = Join-Path ([IO.Path]::GetTempPath()) ('msi-service-table-' + [guid]::NewGuid().ToString('N') + '.log')
    Set-Content -LiteralPath $judgedLog -Encoding Unicode -Value @(
        "MSI (s) (D8:0C) [$(& $stamp 0)]: RESTART MANAGER: Disabled by MSIRESTARTMANAGERCONTROL property; Windows Installer will use the built-in FilesInUse functionality.",
        "MSI (s) (D8:AC) [$(& $stamp 1)]: RESTART MANAGER: Session opened.",
        "MSI (s) (D8:AC) [$(& $stamp 1)]: RESTART MANAGER: Will attempt to shut down and restart applications in no UI modes.",
        "MSI (c) (80:EC) [$(& $stamp 1)]: RESTART MANAGER: Session opened.",
        "MSI (s) (D8:AC) [$(& $stamp 2)]: RESTART MANAGER: Shut down the nested session's applications.",
        "MSI (s) (D8:AC) [$(& $stamp 32)]: Doing action: FastCacheNodeStartService",
        "MSI (s) (D8:AC) [$(& $stamp 33)]: RESTART MANAGER: Restarted FastCached.",
        "MSI (s) (D8:AC) [$(& $stamp 38)]: Product: fastcached -- Installation completed successfully.")
    try {
        $judged = & $record 'NodeSelected' '' 5 0 @()
        $judged.Log = $judgedLog
        # Invoke-Msiexec's own shape: the judgement's output captured as Write-Host's argument.
        $shown = @(& {
                try { Write-Host (Invoke-TransactionJudgement -Transaction $judged -ReadEvents { @(& $termination 7031 34 'FastCached') }) } catch { 'refused' }
            } 6>&1 | ForEach-Object { "$_" })
        if ($shown -notcontains 'refused') { throw "log on refusal: the judgement did not refuse ($($shown.Count) line(s))" }
        $text = $shown -join "`n"
        foreach ($want in @('--- the tail ---\n(MSI [^\n]*\n)*MSI [^\n]*Doing action: FastCacheNodeStartService\n', 'RESTART MANAGER: Shut down the nested session''s applications',
                '--- the actions from 30 s before to 5 s after each finding .*---\n.*Doing action: FastCacheNodeStartService\n.*RESTART MANAGER: Restarted FastCached\.\n.*Product: fastcached -- Installation completed successfully\.')) {
            if ($text -notmatch $want) { throw "log on refusal: nothing shown matches '$want' in:`n$text" }
        }
        if (@(Show-MsiLog $judgedLog 6>$null).Count -ne 0) { throw 'log on refusal: Show-MsiLog wrote to the output stream' }
        Pass 'log on refusal: the log reaches the host from inside a captured judgement, with every RESTART MANAGER line and the actions around each finding'
    } finally {
        Remove-Item -LiteralPath $judgedLog -Force -ErrorAction SilentlyContinue
    }
    foreach ($row in @(
            @{ Case = 'a rollback names its expectation and its reason, and judges no start'; Leaves = ''; Expect = 'RollsBack'; Events = @(); Failed = 0
               Match = "against expectation RollsBack -- the transaction fails on purpose.*no start judged; 7036 unchecked" }
            @{ Case = 'the node started with no 7036 is named ABSENT'; Leaves = 'NodeSelected'; Expect = ''; Events = @(); Failed = 0
               Match = 'against row NodeSelected: no unexpected termination, no start of FastCached; 7036 ABSENT' }
            @{ Case = 'the node''s logged start makes 7036 live'; Leaves = 'NodeSelected'; Expect = ''; Events = @(& $entered 30 'FastCacheCompileNode' 'Running'); Failed = 0
               Match = '7036 live' }
            @{ Case = 'failed observation rounds are named'; Leaves = 'NodeAlone'; Expect = ''; Events = @(); Failed = 2
               Match = '5 observation round\(s\); 2 observation round\(s\) FAILED, first: FastCached: Invalid class$' })) {
        $events = $row.Events
        $line = Invoke-TransactionJudgement -Transaction (& $record $row.Leaves $row.Expect 5 $row.Failed @()) -ReadEvents { $events }
        if ($line -notmatch $row.Match) { throw "judgement: $($row.Case): '$line' does not match '$($row.Match)'" }
        Pass "judgement: $($row.Case)"
    }

    # Assert-ServiceTable judges the last transaction ONCE more, over its own longer window.
    $saved = $script:LastMsiTransaction
    try {
        $script:LastMsiTransaction = & $record 'NothingInstalled' '' 5 0 @()
        $late = @(& $termination 7031 60 'FastCached')
        ExpectThrow 'judgement: Assert-ServiceTable judges the transaction again, over the longer window' {
            Assert-ServiceTable -Row NothingInstalled -StableSeconds 0 -TimeoutSeconds 1 -Observe { param($name) $null } -ReadEvents { $late } 6>$null
        } 'disturbed a service the table owns \(row NothingInstalled\)(.|\n)*event 7031 at 2026-10-06T12:13:14'
        Assert-ServiceTable -Row NothingInstalled -StableSeconds 0 -TimeoutSeconds 1 -Observe { param($name) $null } -ReadEvents { $late } 6>$null
        if (-not $script:LastMsiTransaction.Extended) { throw 'judgement: the transaction is not marked judged' }
        Pass 'judgement: once judged over the longer window, a transaction is not judged again'
    } finally {
        $script:LastMsiTransaction = $saved
    }

    # The watch over a real process: it observes until the process exits, keeps only a service seen
    # running under a process, and leaves the exit code readable through the handle taken first --
    # the shape Invoke-Msiexec starts msiexec in. Observed through the seam: no service here is ours.
    $startProbe = {
        $process = Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList '-NoProfile', '-NonInteractive', '-Command', 'Start-Sleep -Milliseconds 1500; exit 3' -PassThru -NoNewWindow
        $null = $process.Handle
        $process }
    $probe = & $startProbe
    $watched = Watch-ServiceProcesses -Process $probe -IntervalMilliseconds 100 -Observe {
        param($name) if ($name -eq 'FastCached') { [pscustomobject]@{ StartMode = 'Manual'; State = 'Running'; ProcessId = 77 } } else { $null } }
    if ($watched.Polls -lt 1 -or $watched.FailedPolls -ne 0 -or @($watched.Observed).Count -ne $watched.Polls -or
        @($watched.Observed | Where-Object { $_.Service -ne 'FastCached' -or $_.ProcessId -ne 77 }).Count -ne 0) {
        throw "watch: $($watched.Polls) poll(s) observed [$(@($watched.Observed | ForEach-Object { "$($_.Service) $($_.ProcessId)" }) -join '; ')]"
    }
    if ($probe.ExitCode -ne 3) { throw "watch: the watched process's exit code read '$($probe.ExitCode)', expected 3" }
    Pass "watch: $($watched.Polls) observation(s) until the process exited, its exit code readable"
    # An observation that FAILS is counted, the watch goes on, and the process is still waited for.
    $probe = & $startProbe
    $script:failuresLeft = 2
    $watched = Watch-ServiceProcesses -Process $probe -IntervalMilliseconds 100 -Observe {
        param($name) if ($script:failuresLeft -gt 0) { $script:failuresLeft--; throw 'Invalid class' }; $null }
    if ($watched.FailedPolls -lt 1 -or $watched.Polls -lt 1 -or $watched.FirstFailure -notmatch '^FastCached(CompileNode)?: Invalid class$' -or -not $probe.HasExited) {
        throw "watch: a failing observation read as $($watched.Polls) round(s), $($watched.FailedPolls) failed ('$($watched.FirstFailure)'), exited $($probe.HasExited)"
    }
    Pass "watch: a failed observation is counted ($($watched.FailedPolls) round(s)) and the watch goes on to the process's exit"

    $expectedCases = 151
    if ($script:SelfTestCases -ne $expectedCases) {
        throw "ran $script:SelfTestCases cases, expected ${expectedCases}: a case was added or lost without this count moving"
    }
    Write-Host "msi-service-table-selftest: PASSED ($script:SelfTestCases cases)"
}

Export-ModuleMember -Function Get-ServiceObservation, Get-ServiceVerdict, Assert-ServiceState, Assert-ServiceTable,
    Get-MsiProperty, Get-MsiCustomActionType, Get-InstalledProductCodes, Assert-InstalledProduct, Show-MsiLog, Invoke-Msiexec, Assert-MsiLog,
    Get-MsiRestartManagerLines, Show-MsiRestartManagerLines, Get-MsiLogTimeOfDay, Get-MsiLinesAround, Show-MsiLogAroundFindings,
    Get-MsiActionRanPattern, Get-ServiceStabilityVerdict, Get-UpgradeVersionVerdict, Remove-FastCacheMachineState,
    Assert-NodeStatePrivate, Get-NodeServiceAccessVerdict, Get-DirectoryOwnerVerdict, Get-NodeFirewallVerdict, Assert-NodeFirewall,
    Get-NodeFirewallScopeVerdict, Assert-NodeFirewallScope, Get-NodeRegistrationArgumentVerdict,
    Assert-NodeRegistrationArgument, Get-MsiActionOrderVerdict, Assert-MsiActionOrder,
    Get-NodeRegistrationLacksVerdict, Assert-NodeRegistrationLacks, Get-FirewallGroupEmptyVerdict,
    Assert-FirewallGroupEmpty, Format-FirewallRuleLine, Get-FirewallGroupSnapshot, Get-FirewallGroupUnchangedVerdict,
    Assert-FirewallGroupUnchanged, Split-RegisteredCommandLine, Show-ServiceDiagnosis, Get-InstallationSnapshot, Compare-InstallationSnapshot, New-MsiControlCopy,
    Watch-ServiceProcesses, ConvertTo-ServiceControlEvent, Get-ServiceControlEvents, Get-TransactionExpectation,
    Get-TransactionServiceVerdict, Get-Witness7036State,
    Invoke-MsiServiceTableSelfTest
