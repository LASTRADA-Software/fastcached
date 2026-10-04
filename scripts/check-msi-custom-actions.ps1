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
#      that 0 honest, 1460 while a process DOES run from it;
#   6. the actions that must run BEFORE the old product's removal (`BeforeOldProductRemoval`) are
#      scheduled there: a registration copied after 0.3.0's uninstall-service had deleted it was
#      no copy, and a failed upgrade then left 0.3.0 registered with a command line it refuses;
#   7. the key rollback state lives in -- read off FastCacheClearRollbackState, and the one key
#      every action naming rollback state uses -- is covered by NO registry element of the package:
#      one the package owns is deleted by the old product's uninstall, which an upgrade runs AFTER
#      the copy (#6), so the next upgrade would lose the copy this one fixed. It reads the
#      fragment ONLY, and fails OPEN for a registry element CPack's generated sources add;
#   8. that key is deleted where nothing else deletes it -- last in a failed transaction's
#      rollback, after every action that reads it, and on an uninstall that is not an upgrade's.
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

# The actions that must run before RemoveExistingProducts, in this order: the rollback state is
# emptied, then each registration is copied while the old product's is still there.
$BeforeOldProductRemoval = @('FastCacheClearRollbackState', 'FastCachedStashRegistration', 'FastCacheNodeStashRegistration')

$failures = [System.Collections.Generic.List[string]]::new()
function Fail([string] $what) { $failures.Add($what); Write-Host "FAIL $what" }
function Pass([string] $what) { Write-Host "ok   $what" }

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("msi-custom-actions-" + [guid]::NewGuid().ToString('N'))
# A root with a SPACE in it, as Program Files has, and absent until a phase says it exists.
$root = Join-Path $scratch 'Program Files Probe\fastcached\'
$system64 = Join-Path $env:SystemRoot 'System32\'
$knownDirectories = @{ INSTALL_ROOT = $root; System64Folder = $system64 }

# Windows Installer's formatting, for what these targets use: [Property] and [\x]. A property
# no row sets formats to nothing, as Windows Installer formats it.
function Format-Target([string] $text) {
    return [regex]::Replace($text, '\[(\\.|[A-Za-z_][A-Za-z0-9_.]*)\]', {
            param($m)
            $token = $m.Groups[1].Value
            if ($token.StartsWith('\')) { return $token.Substring(1) }
            if ($knownDirectories.ContainsKey($token)) { return $knownDirectories[$token] }
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
        if ($phase -eq 'RootAbsent') { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
    }
} finally {
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# 6. Before the old product's removal, and in order. Each must END on RemoveExistingProducts and
# be anchored Before the next, so WiX numbers them ahead of it whatever MajorUpgrade gave it.
foreach ($index in 0..($BeforeOldProductRemoval.Count - 1)) {
    $id = $BeforeOldProductRemoval[$index]
    $standard = Get-StandardAnchor $id
    $next = if ($index -lt $BeforeOldProductRemoval.Count - 1) { "Before:$($BeforeOldProductRemoval[$index + 1])" } else { 'Before:RemoveExistingProducts' }
    if ($standard -ne 'Before:RemoveExistingProducts' -or $schedule[$id] -ne $next) {
        Fail "$id is scheduled '$($schedule[$id])', ending on '$standard'; it must be '$next', before RemoveExistingProducts, or an upgrade copies a registration the old product has already deleted"
    } else {
        Pass "$id runs before the old product's removal ($next)"
    }
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
    # transaction's rollback runs in reverse, so the rollback delete is scheduled FIRST -- before
    # the clear, before RemoveExistingProducts -- and every action that reads the key (the exact
    # restores, the restarts), scheduled after InstallFiles, runs before it in rollback.
    $undo = $actions | Where-Object { $_.Id -eq 'FastCacheUndoRollbackState' }
    if ($null -eq $undo -or $undo.Execute -ne 'rollback' -or $schedule['FastCacheUndoRollbackState'] -ne 'Before:FastCacheClearRollbackState') {
        Fail "FastCacheUndoRollbackState must be a rollback action scheduled Before FastCacheClearRollbackState, so it runs LAST in a rollback; it is '$($undo.Execute)' at '$($schedule['FastCacheUndoRollbackState'])'"
    } else {
        # Every OTHER rollback action that names rollback state reads it, so each is derived from
        # the table rather than listed again: a new one is ordered by this check the day it is added.
        $readers = @($actions | Where-Object {
                $RollbackStateActions -contains $_.Id -and $_.Execute -eq 'rollback' -and $_.Id -ne 'FastCacheUndoRollbackState'
            } | ForEach-Object { $_.Id })
        if ($readers.Count -eq 0) { Fail 'no rollback action reads the rollback key, so the ordering below judged nothing' }
        $early = @($readers | Where-Object { (Get-StandardAnchor $_) -ne 'After:InstallFiles' })
        if ($early.Count -gt 0) { Fail "these read the rollback key but are not scheduled after InstallFiles, so a rollback could run them after its delete: $($early -join ', ')" }
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

# Positive controls: a walk that found nothing reports nothing wrong about it.
foreach ($id in $RunnableActions.Keys) {
    if (-not ($actions | Where-Object { $_.Id -eq $id })) { Fail "$id is in RunnableActions and no longer in the fragment" }
}
if ($parsed -eq 0) { Fail 'no PowerShell body was found to parse, so step 4 proved nothing' }
if ($started -ne $RunnableActions.Count) { Fail "started $started of $($RunnableActions.Count) runnable actions" }

Write-Host "msi-custom-action-commands: $($actions.Count) action(s), $parsed PowerShell bodies parsed, $started started, $($failures.Count) failure(s)"
if ($failures.Count -gt 0) { exit 1 }
