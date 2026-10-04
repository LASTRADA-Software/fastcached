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
#      that 0 honest, 1460 while a process DOES run from it.
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
function Get-Phase([string] $action, [int] $depth = 0) {
    if ($depth -gt 200) { throw "the schedule of $action does not reach a standard action" }
    $anchor = $schedule[$action]
    if ($null -eq $anchor) { return $null }
    if ($AnchorPhases.ContainsKey($anchor)) { return $AnchorPhases[$anchor] }
    $other = $anchor.Substring($anchor.IndexOf(':') + 1)
    if (-not $schedule.ContainsKey($other)) { throw "$action is anchored on '$anchor', which AnchorPhases has no row for" }
    return Get-Phase $other ($depth + 1)
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

# Positive controls: a walk that found nothing reports nothing wrong about it.
foreach ($id in $RunnableActions.Keys) {
    if (-not ($actions | Where-Object { $_.Id -eq $id })) { Fail "$id is in RunnableActions and no longer in the fragment" }
}
if ($parsed -eq 0) { Fail 'no PowerShell body was found to parse, so step 4 proved nothing' }
if ($started -ne $RunnableActions.Count) { Fail "started $started of $($RunnableActions.Count) runnable actions" }

Write-Host "msi-custom-action-commands: $($actions.Count) action(s), $parsed PowerShell bodies parsed, $started started, $($failures.Count) failure(s)"
if ($failures.Count -gt 0) { exit 1 }
