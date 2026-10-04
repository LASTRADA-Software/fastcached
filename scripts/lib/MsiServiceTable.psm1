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
# Windows host runs and a name no host has, and the package reader over an MSI
# database it creates. It installs nothing. Its two blind spots both fail OPEN:
#
#   * `Get-InstalledProductCodes` is exercised only for an UpgradeCode nothing is
#     installed under, because a non-empty answer needs an installed product.
#   * The table's rows are checked for VOCABULARY only -- a known start mode and a
#     known state -- never against the fragment. A row that is wrong the same way
#     the fragment is wrong (both saying fastcached stays auto beside the node,
#     say) passes here and passes the packaging job, because there the one agrees
#     with the other. Only a reader of both catches that.
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
function Assert-ServiceTable {
    param(
        [Parameter(Mandatory)] [string] $Row,
        [string] $Log = '',
        [int] $TimeoutSeconds = 30,
        [int] $StableSeconds = 5
    )
    if (-not $script:ServiceTable.Contains($Row)) {
        throw "no service table row '$Row'; the rows are: $($script:ServiceTable.Keys -join ', ')"
    }
    Write-Host "service table: $Row"
    try {
        Assert-ServiceState -Expect $script:ServiceTable[$Row] -TimeoutSeconds $TimeoutSeconds -StableSeconds $StableSeconds
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

# Reads one row of a package's Property table.
#
# Every COM object is released before returning, so the package is not held open
# when msiexec or cpack next needs it.
#
# @param Path The .msi file.
# @param Name The property.
# @return Its value; a package without it is refused by name.
function Get-MsiProperty([string] $Path, [string] $Name) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $db = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
    try {
        $view = $db.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $db,
            @("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$Name'"))
        try {
            $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null) | Out-Null
            $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
            if (-not $record) { throw "$Path carries no $Name property" }
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
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
    }
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

# Does the node service's command line carry NO token for @p Prefix? The other direction of
# the verdict above, for a flag an earlier package registered and this node refuses at every
# start: a registration still carrying it was never replaced.
# @param ImagePath The registration's command line.
# @param Prefix The flag, e.g. `--scheduler`: a token equal to it, or it followed by `=`.
# @return $null when no token carries it, else the command line that does.
function Get-NodeRegistrationAbsentVerdict([string] $ImagePath, [string] $Prefix) {
    $tokens = @([regex]::Matches($ImagePath, '"[^"]*"|\S+') | ForEach-Object { $_.Value.Trim('"') })
    $carried = @($tokens | Where-Object { $_ -ceq $Prefix -or $_.StartsWith("$Prefix=", [StringComparison]::Ordinal) })
    if ($carried.Count -eq 0) { return $null }
    return "the node's registration still carries $Prefix, so it is the one an earlier package made: $ImagePath"
}

# The node's REAL registration carries no @p Prefix token. Fails CLOSED on no registration.
function Assert-NodeRegistrationLacks([string] $Prefix) {
    $svc = Get-CimInstance Win32_Service -Filter "Name='FastCacheCompileNode'"
    if (-not $svc) { throw "no FastCacheCompileNode service is registered, so nothing says it lacks $Prefix" }
    if ($verdict = Get-NodeRegistrationAbsentVerdict $svc.PathName $Prefix) { throw $verdict }
    Write-Host "FastCacheCompileNode carries no $Prefix`: $($svc.PathName)"
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
# the self-test cannot reach it; that is a third blind spot of the same kind as the
# two named at the top of this module, and it fails CLOSED -- it throws.
#
# BOTH directions: nothing broad reads or plants, the list is protected, its owner
# keeps only READ_CONTROL, and the service still reaches its key with read but not
# the right to rewrite the list.
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
                     Where-Object { $_.IdentityReference.Value -eq 'S-1-3-4' -and $_.AccessControlType -eq 'Allow' })
    $beyondReading = (-bnot ([int64]$rights::ReadPermissions -bor [int64]$rights::Synchronize)) -band $lowWord
    if ($ownerRights.Count -eq 0) { throw "$state carries no OWNER RIGHTS entry, so its owner keeps WRITE_DAC" }
    if (@($ownerRights | Where-Object { ((([int64]$_.FileSystemRights) -band $lowWord) -band $beyondReading) -ne 0 }).Count -gt 0) {
        throw "$state lets its owner do more than read the access list: $($ownerRights.FileSystemRights)"
    }

    # The service reaches its key -- with READ, and NOT the right to rewrite the list (M1):
    # this process compiles input that arrived over the network.
    $serviceSid = (New-Object Security.Principal.NTAccount 'NT SERVICE\FastCacheCompileNode').Translate([Security.Principal.SecurityIdentifier]).Value
    $own = @((Get-Acl $key).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) |
             Where-Object { $_.IdentityReference.Value -eq $serviceSid -and $_.AccessControlType -eq 'Allow' })
    if ($own.Count -eq 0) { throw 'nothing grants NT SERVICE\FastCacheCompileNode access to its own key' }
    if (@($own | Where-Object { (([int64]$_.FileSystemRights) -band [int64]$rights::ReadData) -ne 0 }).Count -eq 0) {
        throw "NT SERVICE\FastCacheCompileNode cannot read its own key: $($own.FileSystemRights)"
    }
    $writeList = [int64]$rights::ChangePermissions -bor [int64]$rights::TakeOwnership
    if (@($own | Where-Object { (([int64]$_.FileSystemRights) -band $writeList) -ne 0 }).Count -gt 0) {
        throw "NT SERVICE\FastCacheCompileNode may rewrite its key's access list: $($own.FileSystemRights)"
    }
    Write-Host "$state and its identity key answer to nobody but SYSTEM, Administrators and the service"
}

# ---------------------------------------------------------------------------
# msiexec and its log
# ---------------------------------------------------------------------------

# The lines of a verbose log that name what happened, since its tail is only the
# property dump.
#
# @param Path The log.
function Show-MsiLog([string] $Path) {
    Write-Host "===== $Path (relevant lines) ====="
    if (-not (Test-Path $Path)) { Write-Host "no log at $Path"; return }
    # Every line naming one of this package's service actions, from the whole log: the tail
    # below is the property dump of a transaction that ended, and an action that ran with
    # Return="ignore" -- the node's registration among them -- leaves its failure only here.
    Write-Host '--- the service actions ---'
    Select-String -Path $Path -Pattern 'FastCache\w*(Service|ForNode|Leftover)\b' |
        Where-Object { $_.Line -match 'Action (start|ended)|returned actual error|CustomAction' } | ForEach-Object { $_.Line }
    Write-Host '--- the tail ---'
    Select-String -Path $Path -Pattern `
        'Action (start|ended)', 'CustomAction', 'ServiceControl', 'FastCache', 'returned actual error',
        'Note: 1: 1(4|7)[0-9][0-9]', 'Installation (success|failed)', 'error' |
        Select-Object -Last 60 | ForEach-Object { $_.Line }
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
function Invoke-Msiexec {
    param(
        [Parameter(Mandatory)] [ValidateSet('/i', '/x')] [string] $Operation,
        [Parameter(Mandatory)] [string] $Package,
        [Parameter(Mandatory)] [string] $Log,
        [string[]] $Properties = @(),
        [int[]] $Accept = @(0),
        [hashtable] $Notice = @{},
        [Parameter(Mandatory)] [string] $What
    )
    $logPath = [IO.Path]::GetFullPath($Log, (Get-Location).Path)
    $arguments = @($Operation, "`"$Package`"", '/qn', '/l*v', "`"$logPath`"") + $Properties
    Write-Host "$What`: msiexec $($arguments -join ' ')"
    $p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList $arguments
    if ($p.ExitCode -notin $Accept) {
        Show-MsiLog $logPath
        # A checked service action that failed rolled the transaction back, and the service's
        # own events are where its refusal was written: they outlive the rollback.
        foreach ($name in 'FastCacheCompileNode', 'FastCached') { Show-ServiceDiagnosis $name }
        throw "$What exited $($p.ExitCode); accepted: $($Accept -join ', ')"
    }
    Write-Host "$What exited $($p.ExitCode)"
    if ($Notice.ContainsKey($p.ExitCode)) { Write-Host "::notice::$What exited $($p.ExitCode): $($Notice[$p.ExitCode])" }
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
                "INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('UpgradeCode', '{22222222-2222-2222-2222-222222222222}')")) {
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
    # directory. The rest of that function reads a live ACL and is the module's third stated
    # blind spot; this pure part is not.
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
        if ($v = Get-NodeRegistrationAbsentVerdict $old030 '--scheduler') { throw $v }
    } 'still carries --scheduler'
    if ($null -ne (Get-NodeRegistrationAbsentVerdict '"C:\Program Files\x.exe" --daemon --schedulers-seen=1 --node-id=a' '--scheduler')) {
        throw 'absent: a different flag sharing the prefix was taken for it'
    }
    Pass 'absent: a registration without it passes, and a longer flag name is not it'

    $expectedCases = 62
    if ($script:SelfTestCases -ne $expectedCases) {
        throw "ran $script:SelfTestCases cases, expected ${expectedCases}: a case was added or lost without this count moving"
    }
    Write-Host "msi-service-table-selftest: PASSED ($script:SelfTestCases cases)"
}

Export-ModuleMember -Function Get-ServiceObservation, Get-ServiceVerdict, Assert-ServiceState, Assert-ServiceTable,
    Get-MsiProperty, Get-InstalledProductCodes, Assert-InstalledProduct, Show-MsiLog, Invoke-Msiexec, Assert-MsiLog,
    Get-MsiActionRanPattern, Get-ServiceStabilityVerdict, Get-UpgradeVersionVerdict, Remove-FastCacheMachineState,
    Assert-NodeStatePrivate, Get-DirectoryOwnerVerdict, Get-NodeFirewallVerdict, Assert-NodeFirewall,
    Get-NodeFirewallScopeVerdict, Assert-NodeFirewallScope, Get-NodeRegistrationArgumentVerdict,
    Assert-NodeRegistrationArgument, Get-MsiActionOrderVerdict, Assert-MsiActionOrder,
    Get-NodeRegistrationAbsentVerdict, Assert-NodeRegistrationLacks, Split-RegisteredCommandLine, Show-ServiceDiagnosis, Invoke-MsiServiceTableSelfTest
