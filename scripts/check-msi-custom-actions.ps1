# SPDX-License-Identifier: Apache-2.0
#
# Every custom action of the MSI, run the way Windows Installer runs it -- without installing.
#
# A type 34 action (Directory + ExeCommand) is a command line Windows Installer FORMATS and then
# starts with CreateProcess, IN its Directory. Nothing in the WiX build looks at either half: a
# quote PowerShell parses differently, a `[` the formatter swallows, or a working directory that
# does not exist when the action runs all build a package that fails only on a Windows runner,
# minutes into its slowest job. The last one did: FastCacheAwaitServiceExit was started in
# INSTALL_ROOT before InstallFiles, after an upgrade from 0.3.0 had removed that directory, so the
# process was never created and the upgrade failed in under 100 ms (round 5).
#
# So, per action, from packaging/windows/service-actions.xml:
#   1. its PHASE, from the sequence: before InstallFiles the install root may not exist (an
#      upgrade's old product has just removed it), after it the root does (`AnchorPhases`);
#   2. its target, formatted as Windows Installer formats it ([Property], [\x] escapes);
#   3. its working directory, from its Directory in that phase -- an action that would be started
#      in a directory that does not exist is a FAILURE, whatever its command;
#   4. a PowerShell body is parsed by Windows PowerShell 5.1, which is what the action runs;
#   5. the actions that only READ (`RunnableActions`) are started for real, in the directory they
#      would have, and must exit 0 over a root no process runs from -- and, the control that keeps
#      that 0 honest, 1460 while a process DOES run from it, or from the directory the OLD
#      registrations name (R4-1: a 0.3.0 in a custom directory is upgraded into the default root);
#   6. Windows Installer's own rule for RemoveExistingProducts scheduled after InstallInitialize
#      (as MajorUpgrade's afterInstallInitialize does): NO action that writes to the execution
#      script -- deferred, rollback or commit -- may come before it, or the package is refused at
#      once with Error 2613 (round 7, when round 6 had put the registration copies there);
#   7. the key rollback state lives in -- read off FastCacheClearRollbackState, and the one key
#      every action naming rollback state uses -- is covered by NO registry element of the package:
#      one the package owns is deleted by an uninstall, and the state must outlive a failed
#      transaction's rollback until its last step (#8). It reads the
#      fragment ONLY, and fails OPEN for a registry element CPack's generated sources add;
#   8. that key is deleted where nothing else deletes it -- last in a failed transaction's
#      rollback, after every action that reads it, and on an uninstall that is not an upgrade's;
#   9. every action that can FAIL the transaction (in the script, Return="check") comes after
#      both exact restores, so its failure rolls BOTH services back: an upgrade from 0.3.0 has no
#      other way back, since 0.3.0's uninstall deleted them and 0.3.0 ships no rollback. The
#      window from RemoveExistingProducts through WriteRegistryValues cannot be covered, and the
#      one checked action of ours in it is a stated residual (`ChecksBeforeTheRestores`);
#  10. a rollback action that STARTS a service is scheduled before InstallFiles, so the rollback,
#      which runs in reverse, starts it only after InstallFiles' rollback restored the files. That
#      holds for a maintenance transaction; in an upgrade the old files return only with
#      RemoveExistingProducts' rollback, after the restarts, and no mark is written there to start
#      anything (StopServices has stopped both services first).
# Every other action changes a service, the registry or the firewall, and is never started here.
#
# Usage: pwsh -NoProfile -File scripts/check-msi-custom-actions.ps1 -SourceDir <repository root>
# Exit 0 when every action passes; non-zero, naming each failure, otherwise.

param([Parameter(Mandatory)] [string] $SourceDir)
$ErrorActionPreference = 'Stop'

$fragmentPath = Join-Path $SourceDir 'packaging/windows/service-actions.xml'
[xml] $fragment = Get-Content -Raw -LiteralPath $fragmentPath

# Where an action anchored on a standard action runs, relative to the install root existing. An
# anchor with no row is refused rather than guessed: a new one is a decision about that table.
$AnchorPhases = @{
    'Before:RemoveExistingProducts' = 'RootAbsent' # a fresh install has no root yet
    'After:WriteRegistryValues' = 'RootPresent'
    'Before:InstallFiles' = 'RootAbsent'   # after RemoveExistingProducts, before the files
    'After:InstallFiles'  = 'RootPresent'
    'Before:RemoveFiles'  = 'RootPresent'  # an uninstall, while the files are still there
}

# The actions that only READ, and are therefore started for real. Each names the processes its
# control runs from the install root, and the bound its control shortens.
$RunnableActions = @{
    FastCacheAwaitServiceExit  = @{ ControlImages = @('fastcached.exe', 'fastcache-compile-node.exe') }
    FastCachedAwaitExitForNode = @{ ControlImages = @('fastcached.exe') }
}

# The standard actions an anchor of the fragment ends on, with their InstallExecuteSequence
# numbers. RemoveExistingProducts is NOT in Windows Installer's standard table at a fixed place:
# CPack's WIX.template.in declares `<MajorUpgrade Schedule="afterInstallInitialize">`, which puts it
# directly after InstallInitialize -- the premise of step 6, stated here because the template is
# not in this repository to read. An anchor on a standard action with no row is refused.
$StandardSequence = @{
    InstallValidate        = 1400
    InstallInitialize      = 1500
    RemoveExistingProducts = 1501
    RemoveFiles            = 3500
    InstallFiles           = 4000
    WriteRegistryValues    = 5000
}

$failures = [System.Collections.Generic.List[string]]::new()
function Fail([string] $what) { $failures.Add($what); Write-Host "FAIL $what" }
function Pass([string] $what) { Write-Host "ok   $what" }

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("msi-custom-actions-" + [guid]::NewGuid().ToString('N'))
# A root with a SPACE in it, as Program Files has, and absent until a phase says it exists.
$root = Join-Path $scratch 'Program Files Probe\fastcached\'
$system64 = Join-Path $env:SystemRoot 'System32\'
$knownDirectories = @{ INSTALL_ROOT = $root; System64Folder = $system64 }
# Properties a case sets, beside the directories; empty but for the old-registration control below.
$knownProperties = @{}

# Windows Installer's formatting, for what these targets use: [Property] and [\x]. A property
# no row sets formats to nothing, as Windows Installer formats it.
function Format-Target([string] $text) {
    return [regex]::Replace($text, '\[(\\.|[A-Za-z_][A-Za-z0-9_.]*)\]', {
            param($m)
            $token = $m.Groups[1].Value
            if ($token.StartsWith('\')) { return $token.Substring(1) }
            if ($knownDirectories.ContainsKey($token)) { return $knownDirectories[$token] }
            if ($knownProperties.ContainsKey($token)) { return $knownProperties[$token] }
            return ''
        })
}

# The phase of every scheduled action, resolved through After/Before chains to a standard anchor.
$schedule = @{}
foreach ($custom in $fragment.SelectNodes('//InstallExecuteSequence/Custom')) {
    $anchor = if ($custom.HasAttribute('After')) { "After:$($custom.After)" } else { "Before:$($custom.Before)" }
    $schedule[$custom.Action] = $anchor
}
# The standard action a scheduled action's chain ends on, as `<After|Before>:<action>`.
function Get-StandardAnchor([string] $action, [int] $depth = 0) {
    if ($depth -gt 200) { throw "the schedule of $action does not reach a standard action" }
    $anchor = $schedule[$action]
    if ($null -eq $anchor) { return $null }
    $other = $anchor.Substring($anchor.IndexOf(':') + 1)
    if (-not $schedule.ContainsKey($other)) { return $anchor }
    return Get-StandardAnchor $other ($depth + 1)
}
function Get-Phase([string] $action) {
    $anchor = Get-StandardAnchor $action
    if ($null -eq $anchor) { return $null }
    if (-not $AnchorPhases.ContainsKey($anchor)) { throw "$action is anchored on '$anchor', which AnchorPhases has no row for" }
    return $AnchorPhases[$anchor]
}

function Start-Target([string] $commandLine, [string] $workingDirectory) {
    if ($commandLine -notmatch '^"([^"]+)"\s+(.*)$') { throw "not a quoted program: $commandLine" }
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Matches[1], $Matches[2])
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = $workingDirectory
    $process = [System.Diagnostics.Process]::Start($psi)
    if (-not $process.WaitForExit(60000)) { $process.Kill(); throw "still running after 60 s: $commandLine" }
    return $process.ExitCode
}

$actions = @($fragment.SelectNodes('//CustomAction') | Where-Object { $_.HasAttribute('ExeCommand') })
$parsed = 0
$started = 0
try {
    foreach ($action in $actions) {
        $id = $action.Id
        $phase = Get-Phase $id
        if ($null -eq $phase) { continue } # declared, never scheduled: nothing runs it
        $target = Format-Target $action.ExeCommand
        if ($target -match '\[[A-Za-z_\\]') { Fail "$id keeps a token Windows Installer would format: $target"; continue }

        # 3. The working directory, in the action's phase.
        $directory = $action.Directory
        if (-not $knownDirectories.ContainsKey($directory)) { Fail "$id runs in Directory '$directory', which this check has no row for"; continue }
        $directoryExists = ($directory -ne 'INSTALL_ROOT') -or ($phase -eq 'RootPresent')
        if (-not $directoryExists) {
            Fail "$id is started in INSTALL_ROOT before InstallFiles, where an upgrade's old product has just removed it: the process cannot be created (Error 1721). Give it a Directory that always exists (System64Folder)."
            continue
        }

        # 4. A PowerShell body, parsed by the PowerShell the action runs.
        if ($target -match 'powershell\.exe"\s.*-Command "(.*)"$') {
            $body = $Matches[1].Replace('\"', '"')
            $bodyFile = Join-Path $scratch "$id.ps1"
            New-Item -ItemType Directory -Force -Path $scratch | Out-Null
            Set-Content -LiteralPath $bodyFile -Value $body -Encoding utf8
            $verdict = & (Join-Path $system64 'WindowsPowerShell\v1.0\powershell.exe') -NoProfile -NonInteractive -Command `
                "`$e=`$null; [void][System.Management.Automation.Language.Parser]::ParseFile('$bodyFile', [ref]`$null, [ref]`$e); if (`$e.Count) { `$e | ForEach-Object { `$_.Message }; exit 1 }"
            if ($LASTEXITCODE -ne 0) { Fail "$id does not parse in Windows PowerShell 5.1: $verdict" } else { $parsed++; Pass "$id parses in Windows PowerShell 5.1" }
        }

        # 5. The actions that only read, started for real in the directory they would have.
        if (-not $RunnableActions.ContainsKey($id)) { continue }
        $workingDirectory = $knownDirectories[$directory]
        if ($phase -eq 'RootPresent') { New-Item -ItemType Directory -Force -Path $root | Out-Null }
        else { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
        try {
            $code = Start-Target $target $workingDirectory
        } catch {
            Fail "$id could not be started in $workingDirectory ($phase): $($_.Exception.Message)"
            continue
        }
        $started++
        if ($code -ne 0) { Fail "$id exited $code over a root no process runs from; 0 is the no-op" } else { Pass "$id exits 0 over a root no process runs from ($phase)" }

        # The control: a process running from the root keeps the wait waiting, to its (shortened) bound.
        $bin = Join-Path $root 'bin'
        New-Item -ItemType Directory -Force -Path $bin | Out-Null
        $holders = @()
        foreach ($image in $RunnableActions[$id].ControlImages) {
            $copy = Join-Path $bin $image
            Copy-Item -Force (Join-Path $system64 'PING.EXE') $copy
            $holders += Start-Process -FilePath $copy -ArgumentList '-n', '60', '127.0.0.1' -PassThru -WindowStyle Hidden
        }
        try {
            $shortened = [regex]::Replace($target, '-gt \d+\)', '-gt 2)')
            $held = Start-Target $shortened $workingDirectory
            if ($held -ne 1460) { Fail "$id exited $held while a process ran from the root; 1460 is the timed-out wait" } else { Pass "$id waits while a process runs from the root, and says 1460 at its bound" }
        } finally {
            foreach ($holder in $holders) { Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue; $holder.WaitForExit() }
        }

        # R4-1's control: the OLD registrations named another directory -- a 0.3.0 installed
        # elsewhere, upgraded into the default root -- and a process running from THERE keeps the
        # wait waiting too, with nothing running from the new root. Each registration in a spelling
        # AppSearch was measured or documented to return (service-actions.xml): fastcached's bare,
        # the node's raw with `#%`, and both with a space in the path, as a custom one may have.
        $oldBin = Join-Path $scratch 'Old Custom Root\fastcached\bin'
        New-Item -ItemType Directory -Force -Path $oldBin | Out-Null
        $knownProperties['FASTCACHED_IMAGEPATH'] = '"' + (Join-Path $oldBin 'fastcached.exe') + '" --daemon --service-name=FastCached'
        $knownProperties['FASTCACHE_NODE_IMAGEPATH'] = '#%"' + (Join-Path $oldBin 'fastcache-compile-node.exe') + '" --daemon'
        try {
            $oldTarget = Format-Target $action.ExeCommand
            $quiet = Start-Target $oldTarget $workingDirectory
            if ($quiet -ne 0) { Fail "$id exited $quiet with old registrations named and no process running; 0 is the no-op" }
            $holders = @()
            foreach ($image in $RunnableActions[$id].ControlImages) {
                $copy = Join-Path $oldBin $image
                Copy-Item -Force (Join-Path $system64 'PING.EXE') $copy
                $holders += Start-Process -FilePath $copy -ArgumentList '-n', '60', '127.0.0.1' -PassThru -WindowStyle Hidden
            }
            try {
                $held = Start-Target ([regex]::Replace($oldTarget, '-gt \d+\)', '-gt 2)')) $workingDirectory
                if ($held -ne 1460) {
                    Fail "$id exited $held while a process ran from the directory the OLD registration named; 1460 is the timed-out wait (R4-1: an upgrade from a 0.3.0 in a custom directory)"
                } elseif ($quiet -eq 0) {
                    Pass "$id waits for a process running from the directory the old registration named, not only the new root"
                }
            } finally {
                foreach ($holder in $holders) { Stop-Process -Id $holder.Id -Force -ErrorAction SilentlyContinue; $holder.WaitForExit() }
            }
        } finally {
            $knownProperties.Clear()
        }
        if ($phase -eq 'RootAbsent') { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# 6. Error 2613's rule: with RemoveExistingProducts after InstallInitialize, nothing that writes
# to the execution script may come between them. Every deferred, rollback or commit action must
# therefore END on a standard anchor that places it after RemoveExistingProducts: After an action
# numbered at or after it, or Before one numbered after it. An immediate action writes no script
# operation and may sit anywhere.
$inScript = @($actions | Where-Object { $_.Execute -in @('deferred', 'rollback', 'commit') -and $schedule.ContainsKey($_.Id) })
if ($inScript.Count -eq 0) { Fail 'no deferred, rollback or commit action is scheduled, so the Error 2613 rule judged nothing' }
$removal = $StandardSequence['RemoveExistingProducts']
foreach ($action in $inScript) {
    $anchor = Get-StandardAnchor $action.Id
    $relation, $standard = $anchor -split ':', 2
    if (-not $StandardSequence.ContainsKey($standard)) { Fail "$($action.Id) ends on '$anchor', a standard action StandardSequence has no row for"; continue }
    $legal = if ($relation -eq 'After') { $StandardSequence[$standard] -ge $removal } else { $StandardSequence[$standard] -gt $removal }
    if (-not $legal) {
        Fail "$($action.Id) is a $($action.Execute) action ending on '$anchor', so it writes to the execution script between InstallInitialize and RemoveExistingProducts: Windows Installer refuses the package with Error 2613 (RemoveExistingProducts action sequenced incorrectly)"
    }
}
if (-not ($failures | Where-Object { $_ -match 'Error 2613' })) {
    Pass "none of the $($inScript.Count) deferred, rollback or commit actions precedes RemoveExistingProducts (Error 2613's rule)"
}

# Where an action sits relative to the standard actions: a standard action's own number, or just
# after or just before the one its chain ends on. $null for an anchor StandardSequence has no row for.
function Get-AnchorPoint([string] $action) {
    if ($StandardSequence.ContainsKey($action)) { return [double] $StandardSequence[$action] }
    $anchor = Get-StandardAnchor $action
    if ($null -eq $anchor) { return $null }
    $relation, $standard = $anchor -split ':', 2
    if (-not $StandardSequence.ContainsKey($standard)) { return $null }
    return [double] $StandardSequence[$standard] + $(if ($relation -eq 'After') { 0.5 } else { -0.5 })
}

# Does $Earlier run before $Later? True when the fragment's After/Before chains connect them, which
# is the order WiX numbers them in, or when their chains end on different points of the standard
# sequence and $Earlier's comes first; false when nothing ties the two together.
function Test-ScheduledBefore([string] $Earlier, [string] $Later) {
    $earlierPoint = Get-AnchorPoint $Earlier
    $laterPoint = Get-AnchorPoint $Later
    if ($null -ne $earlierPoint -and $null -ne $laterPoint -and $earlierPoint -ne $laterPoint) { return $earlierPoint -lt $laterPoint }
    $seen = @{}
    $queue = [System.Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($Earlier)
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        if ($current -eq $Later) { return $true }
        if ($seen.ContainsKey($current)) { continue }
        $seen[$current] = $true
        foreach ($id in $schedule.Keys) { if ($schedule[$id] -eq "After:$current") { $queue.Enqueue($id) } }
        if ($schedule.ContainsKey($current) -and $schedule[$current].StartsWith('Before:')) { $queue.Enqueue($schedule[$current].Substring(7)) }
    }
    return $false
}

# 7. The rollback state's key, owned by nothing the package installs or removes.
#
# BLIND SPOT, failing OPEN: this reads packaging/windows/service-actions.xml only. CPack generates
# the rest of the package's WiX source at package time, out of this check's reach, so a registry
# element CPack added there that covered the key would pass here. Measured 2026-10-04 on a package
# built from this tree: the generated sources hold no registry element but the fragment's.
#
# Every action that names rollback state, by id, so a lost one AND a new one are both findings.
$RollbackStateActions = @(
    'FastCachedStashRegistration', 'FastCacheNodeStashRegistration',
    'FastCachedStashFromCopy', 'FastCacheNodeStashFromCopy',
    'FastCachedRestoreRegistrationExactly', 'FastCacheNodeRestoreRegistrationExactly',
    'FastCachedStopForNode', 'FastCacheNodeStopForRestart',
    'FastCachedRestartAfterStop', 'FastCacheNodeRestartAfterStop',
    'FastCacheClearRollbackState', 'FastCacheDiscardRollbackState',
    'FastCacheUndoRollbackState', 'FastCacheRemoveRollbackState')

# Does a registry element on $Owned cover $Key -- the key itself, a parent that removes it with
# its subtree, or a child inside it? Registry names are case-INSENSITIVE, so the comparison is too.
function Test-RegistryKeyCovers([string] $Owned, [string] $Key) {
    $owned = $Owned.TrimEnd('\')
    $ignoreCase = [StringComparison]::OrdinalIgnoreCase
    return $owned.Equals($Key, $ignoreCase) -or $Key.StartsWith("$owned\", $ignoreCase) -or $owned.StartsWith("$Key\", $ignoreCase)
}

$failuresBeforeRollbackKey = $failures.Count
$clear = $actions | Where-Object { $_.Id -eq 'FastCacheClearRollbackState' }
if ($null -eq $clear -or $clear.ExeCommand -notmatch 'reg\.exe"?\s+delete\s+HKLM\\(\S+)\s') {
    Fail 'FastCacheClearRollbackState does not delete an HKLM key, so the rollback key could not be read off it'
} else {
    $rollbackKey = $Matches[1]

    # The guard seen both ways, on the key it is about to judge: a parent spelled in another
    # case covers it (Windows reads it as the same key), a sibling sharing its prefix does not.
    $coverageRows = @(
        @{ Owned = ($rollbackKey.Substring(0, $rollbackKey.LastIndexOf('\'))).ToLowerInvariant(); Covers = $true },
        @{ Owned = $rollbackKey.ToUpperInvariant(); Covers = $true },
        @{ Owned = "$rollbackKey\FastCached"; Covers = $true },
        @{ Owned = "${rollbackKey}Elsewhere"; Covers = $false },
        @{ Owned = 'SOFTWARE\fastcached\Installer'; Covers = $false }
    )
    foreach ($row in $coverageRows) {
        if ((Test-RegistryKeyCovers $row.Owned $rollbackKey) -ne $row.Covers) {
            Fail "the coverage test answers $(-not $row.Covers) for '$($row.Owned)' over $rollbackKey; it should answer $($row.Covers)"
        }
    }

    $naming = @($actions | Where-Object { $_.ExeCommand -match 'Rollback' } | ForEach-Object { $_.Id })
    foreach ($id in $RollbackStateActions) { if ($naming -notcontains $id) { Fail "$id is in RollbackStateActions and names no rollback state" } }
    foreach ($id in $naming) { if ($RollbackStateActions -notcontains $id) { Fail "$id names rollback state and is not in RollbackStateActions" } }
    $stray = @($actions | Where-Object { $_.ExeCommand -match 'Rollback' -and $_.ExeCommand -notmatch [regex]::Escape($rollbackKey) })
    foreach ($action in $stray) { Fail "$($action.Id) names rollback state outside $rollbackKey" }
    $registry = @($fragment.SelectNodes('//*[local-name()="RegistryKey" or local-name()="RegistryValue" or local-name()="RemoveRegistryKey" or local-name()="RemoveRegistryValue"]'))
    if ($registry.Count -eq 0) { Fail 'the fragment has no registry element at all, so the ownership check below judged nothing' }
    $covering = @($registry | Where-Object { $_.HasAttribute('Key') -and (Test-RegistryKeyCovers $_.Key $rollbackKey) })
    foreach ($element in $covering) {
        Fail "the package's $($element.LocalName) on $($element.Key) covers the rollback key ${rollbackKey}: the old product's uninstall, which an upgrade runs after the copy, would delete it"
    }
    if ($failures.Count -eq $failuresBeforeRollbackKey) {
        Pass "rollback state lives in $rollbackKey ($($naming.Count) actions), which none of the fragment's $($registry.Count) registry elements covers (CPack's generated sources are not read: fails open there)"
    }

    # 8. The key REMOVED where nothing else removes it, and never before it is read. A failed
    # transaction's rollback runs in reverse, so the rollback delete is scheduled ahead of every
    # action that reads the key (the exact restores, the restarts), and each of them runs before
    # it in a rollback.
    $undo = $actions | Where-Object { $_.Id -eq 'FastCacheUndoRollbackState' }
    if ($null -eq $undo -or $undo.Execute -ne 'rollback' -or -not $schedule.ContainsKey('FastCacheUndoRollbackState')) {
        Fail "FastCacheUndoRollbackState must be a scheduled rollback action, so it runs LAST in a rollback; it is '$($undo.Execute)' at '$($schedule['FastCacheUndoRollbackState'])'"
    } else {
        # Every OTHER rollback action that names rollback state reads it, so each is derived from
        # the table rather than listed again: a new one is ordered by this check the day it is added.
        $readers = @($actions | Where-Object {
                $RollbackStateActions -contains $_.Id -and $_.Execute -eq 'rollback' -and $_.Id -ne 'FastCacheUndoRollbackState'
            } | ForEach-Object { $_.Id })
        if ($readers.Count -eq 0) { Fail 'no rollback action reads the rollback key, so the ordering below judged nothing' }
        $early = @($readers | Where-Object { -not (Test-ScheduledBefore 'FastCacheUndoRollbackState' $_) })
        if ($early.Count -gt 0) { Fail "these read the rollback key but are not scheduled after FastCacheUndoRollbackState, so a rollback could run them after its delete: $($early -join ', ')" }
        else { Pass 'FastCacheUndoRollbackState runs last in a rollback, after every action that reads the key' }
    }
    $remove = $fragment.SelectSingleNode('//InstallExecuteSequence/Custom[@Action="FastCacheRemoveRollbackState"]')
    $removeAction = $actions | Where-Object { $_.Id -eq 'FastCacheRemoveRollbackState' }
    if ($null -eq $remove -or $removeAction.Execute -ne 'deferred' -or $remove.Condition -ne 'REMOVE = "ALL" AND NOT UPGRADINGPRODUCTCODE') {
        Fail "FastCacheRemoveRollbackState must be a deferred action conditioned 'REMOVE = `"ALL`" AND NOT UPGRADINGPRODUCTCODE': an upgrade's removal of the old product must not delete the copy taken before it"
    } else {
        Pass 'FastCacheRemoveRollbackState deletes the key on an uninstall, never in an upgrade''s removal of the old product'
    }
}

# 9. Every checked step behind both exact restores.
#
# THE RESIDUAL, stated: RemoveExistingProducts runs 0.3.0's uninstall, which deletes both services
# and has no rollback; nothing that writes the script may precede it (#6), the registration copies
# do not exist before WriteRegistryValues and the binaries a re-registration runs do not exist
# before InstallFiles. A failure in ANY action from RemoveExistingProducts through
# WriteRegistryValues -- the removal itself, StopServices, DeleteServices, RemoveRegistryValues,
# RemoveFiles, CreateFolders, InstallFiles, WriteRegistryValues -- rolls an upgrade from 0.3.0
# back with NO service registered. Only the checked actions of OURS in that window are rows here. Each such action is a row here with its reason, and a row whose action is
# no longer before the restores, or is gone, is refused as stale.
$ChecksBeforeTheRestores = @{
    FastCacheAwaitServiceExit = 'waits for the OLD product''s processes, so it must precede InstallFiles, which precedes WriteRegistryValues'
}
$restores = @('FastCachedRestoreRegistrationExactly', 'FastCacheNodeRestoreRegistrationExactly')
$checked = @($actions | Where-Object {
        $_.Execute -in @('deferred', 'commit') -and $schedule.ContainsKey($_.Id) -and
        (-not $_.HasAttribute('Return') -or $_.Return -eq 'check')
    })
if ($checked.Count -eq 0) { Fail 'no checked deferred action is scheduled, so step 9 judged nothing' }
$failuresBeforeRestoreOrder = $failures.Count
foreach ($action in $checked) {
    $unarmed = @($restores | Where-Object { -not (Test-ScheduledBefore $_ $action.Id) })
    if ($unarmed.Count -eq 0) {
        if ($ChecksBeforeTheRestores.ContainsKey($action.Id)) { Fail "$($action.Id) is a stated residual of ChecksBeforeTheRestores but comes after both exact restores; delete the row" }
        continue
    }
    if (-not $ChecksBeforeTheRestores.ContainsKey($action.Id)) {
        Fail "$($action.Id) can fail the transaction (Return=`"check`") before the script holds $($unarmed -join ' and '): a failed upgrade from 0.3.0 then rolls back with that service gone. Schedule it after both."
    }
}
foreach ($id in $ChecksBeforeTheRestores.Keys) {
    if (-not ($checked | Where-Object { $_.Id -eq $id })) { Fail "ChecksBeforeTheRestores names $id, which is no longer a checked scheduled action; delete the row" }
}
if ($failures.Count -eq $failuresBeforeRestoreOrder) {
    Pass "$($checked.Count - $ChecksBeforeTheRestores.Count) of $($checked.Count) checked actions come after both exact restores; the other $($ChecksBeforeTheRestores.Count) is a stated residual before WriteRegistryValues"
}

# 10. A rollback that starts a service starts the files that were there. InstallFiles' rollback
# restores the previous files, and runs at InstallFiles' place in reverse: a rollback action
# scheduled AFTER InstallFiles runs BEFORE that restore, so a start there runs the new binary (or
# loads the new runtime DLLs) while the old ones are still to be put back over it.
$starters = @($actions | Where-Object {
        $_.Execute -eq 'rollback' -and $schedule.ContainsKey($_.Id) -and $_.ExeCommand -match '(sc|net)\.exe"?\s+start\s'
    })
if ($starters.Count -eq 0) { Fail 'no scheduled rollback action starts a service, so step 10 judged nothing' }
$failuresBeforeStarters = $failures.Count
foreach ($action in $starters) {
    if (-not (Test-ScheduledBefore $action.Id 'InstallFiles')) {
        Fail "$($action.Id) starts a service in a rollback but is not scheduled before InstallFiles, so the rollback starts it BEFORE InstallFiles' rollback restores the previous files: the new binary runs, or the restore meets a file in use"
    }
}
if ($failures.Count -eq $failuresBeforeStarters) {
    Pass "all $($starters.Count) rollback actions that start a service are scheduled before InstallFiles, so they run after its rollback restored the files"
}

# Positive controls: a walk that found nothing reports nothing wrong about it.
foreach ($id in $RunnableActions.Keys) {
    if (-not ($actions | Where-Object { $_.Id -eq $id })) { Fail "$id is in RunnableActions and no longer in the fragment" }
}
if ($parsed -eq 0) { Fail 'no PowerShell body was found to parse, so step 4 proved nothing' }
if ($started -ne $RunnableActions.Count) { Fail "started $started of $($RunnableActions.Count) runnable actions" }

Write-Host "msi-custom-action-commands: $($actions.Count) action(s), $parsed PowerShell bodies parsed, $started started, $($failures.Count) failure(s)"
if ($failures.Count -gt 0) { exit 1 }
