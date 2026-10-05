# SPDX-License-Identifier: Apache-2.0
#
# The environment a PowerShell e2e fixture starts from -- the PowerShell half of
# `e2e_scrub_fastcache_environment` in `e2e-common.sh`.
#
# The launcher, fastcached and the node read their whole configuration from FASTCACHE_*
# variables, so one this process inherited decides what a fixture measures: an operator's
# FASTCACHE_SCHEDULER dispatches each "local" compile to their fleet, whose objects name a
# worker's scratch directory instead of the fixture's checkout, and FASTCACHE_VERIFY or
# FASTCACHE_NO_DIRECT changes the outcome a case asserts. A fixture sets what it means and
# inherits nothing.
#
# IMPORTING this module clears them, so a fixture gets that by construction rather than by
# remembering a loop: two fixtures carried their own copy, one of them wedged into the
# middle of an unrelated comment, and the rest carried none. `E2EPorts.psm1` imports it, so
# every fixture that draws a port is covered too. Import it BEFORE the fixture's own first
# `$env:FASTCACHE_*` assignment -- which is what "the fixture sets what it means" requires.

# A variable the HARNESS reads and no server or launcher does, which an operator sets on
# purpose and which a fixture must therefore see. None on this side today; the POSIX list
# names why each of its rows is there. A row here is a claim that nothing this project
# ships reads the name.
$script:InheritedKnobs = @()

<#
.SYNOPSIS
    Remove every FASTCACHE_* variable from this process's environment, except the
    harness knobs in `$InheritedKnobs`.
#>
function Clear-FastcacheEnvironment {
    Get-ChildItem Env: |
        Where-Object { $_.Name -like 'FASTCACHE_*' -and $script:InheritedKnobs -notcontains $_.Name } |
        ForEach-Object { Remove-Item -Path "Env:\$($_.Name)" -ErrorAction SilentlyContinue }
}

Clear-FastcacheEnvironment

# ---------------------------------------------------------------------------
# A fixture's launcher records into a state directory of the run's own
# ---------------------------------------------------------------------------
#
# The launcher keeps its statistics in `<state>/fastcache-cc/invocations.log`, and `-z`
# DELETES that file. A fixture that runs `-z` against the developer's state directory
# deletes their statistics; one that merely compiles appends its records to them. Both
# happened: the Windows launcher e2e did the first, and three fixtures the second.
#
# Scoped to the LAUNCHER, never the process. LOCALAPPDATA is not the launcher's alone --
# sccache, the CPM source cache and the VS tooling read it too -- so nothing here changes
# the fixture's own environment. `Use-E2ELauncherState` redirects around ONE launcher
# child and puts the caller's values back before the next line runs, and
# `Assert-E2ELauncherFixture` -- run by each fixture at startup and again by
# `Enter-E2ELauncherState` -- refuses a fixture in which any launcher runs outside it.
#
# The POSIX twin is `e2e_launcher_state_enter` in `scripts/lib/e2e-common.sh`. It reaches
# the same scoping differently -- a shim that sets the variables and `exec`s the
# launcher, since a bash fixture spells its launcher calls in forty shapes -- and it
# reads Stats.cpp with its own reader. `launcher-state-isolation` holds the two readers
# to one answer, and requires every fixture handed a launcher to call one seam or the other.

# Where the launcher decides its state directory. Read, never restated: a base the
# launcher learns to honour later is redirected without anybody remembering this file.
$script:LauncherStateSource = Join-Path $PSScriptRoot "../../src/apps/fastcache-cc/Stats.cpp"

# Where under a state base the launcher keeps its log. RESTATED, because the caller's logs
# are snapshotted at the fixture's startup, before any launcher may be asked;
# `Enter-E2ELauncherState` refuses when the launcher's own answer disagrees.
$script:LauncherStateLogRelative = 'fastcache-cc/invocations.log'

# How many trailing bytes of a present log a snapshot hashes. The launcher only appends and
# deletes, so a smaller size or a changed tail is every way a record can be lost.
$script:LauncherStateTailBytes = 65536

# The caller's logs as the fixture's startup found them; taken by the first
# `Assert-E2ELauncherFixture`, handed to the handle by `Enter-E2ELauncherState`.
$script:LauncherCallerSnapshot = $null

# The command a launcher execution must sit inside.
$script:LauncherStateWrapper = 'Use-E2ELauncherState'

# The invocation log's layout, read once from Stats.cpp by `Get-E2ELauncherLogLayout`.
$script:LauncherLogLayout = $null

<#
.SYNOPSIS
    The invocation log's LAYOUT, read out of Stats.cpp rather than restated: the version this
    build writes (`v` and `CurrentLogVersion`) and the names of `LogColumnTable`'s columns, in
    the order they are written. The POSIX twin is `e2e_launcher_log_layout`.
.DESCRIPTION
    A line the launcher writes opens with that version and then those columns; a line with no
    version is the layout from before versions, the same columns in the same order up to its
    arity. Read by NAME because a column's position is not the launcher's contract, the version
    is: the readers this replaces took the outcome as the first field and the source as the
    fifth, and once the log named its version the outcome read `v2` and the caller-damage check
    compared every record's elapsed time against the run's trees -- reporting a leak as clean.
    Throws rather than guessing when either half is not found, and when the names it read are
    fewer than the table's `LogColumn::` rows: a name outside `[a-z-]+` skipped in silence would
    shift every later column by one.
#>
function Get-E2ELauncherLogLayout([string]$Path = $script:LauncherStateSource) {
    $text = [IO.File]::ReadAllText($Path)
    $version = [regex]::Match($text, 'constexpr unsigned CurrentLogVersion = (\d+);')
    if (-not $version.Success) { throw "no CurrentLogVersion in $Path" }
    $table = [regex]::Match($text, '(?s)LogColumnTable \{ \{(.*?)\r?\n    \} \};')
    if (-not $table.Success) { throw "no LogColumnTable in $Path" }
    $names = @([regex]::Matches($table.Groups[1].Value, '\.name = "([a-z-]+)"') | ForEach-Object { $_.Groups[1].Value })
    if ($names.Count -eq 0) { throw "LogColumnTable in $Path names no column" }
    $rows = [regex]::Matches($table.Groups[1].Value, '\.column = LogColumn::').Count
    if ($names.Count -ne $rows) {
        throw "the invocation log layout in $Path names $($names.Count) column(s) for $rows LogColumn row(s); a name outside [a-z-] would shift every later column"
    }
    return [pscustomobject]@{ Version = "v$($version.Groups[1].Value)"; Columns = $names }
}

<#
.SYNOPSIS
    Column -Name of one invocation-log line under -Layout, with how it was read: `read`, or
    `foreign` for a line naming a version this build does not write, or `malformed` for a line in
    this build's version that does not carry exactly its columns -- which Stats.cpp's own reader
    refuses too. Neither of the last two is read by position.
.DESCRIPTION
    Throws only for a column the layout does not have: that is the caller's mistake, not the
    line's.
#>
function Read-E2ELauncherLogLine([AllowEmptyString()][string]$Line, [string]$Name, $Layout) {
    $index = [array]::IndexOf([string[]]$Layout.Columns, $Name)
    if ($index -lt 0) { throw "the invocation log has no column named '$Name'" }
    $fields = $Line -split "`t"
    if ($fields[0] -match '^v\d+$') {
        if ($fields[0] -ne $Layout.Version) { return [pscustomobject]@{ Status = 'foreign'; Value = '' } }
        if ($fields.Count -ne $Layout.Columns.Count + 1) { return [pscustomobject]@{ Status = 'malformed'; Value = '' } }
        $index++
    }
    $value = if ($index -lt $fields.Count) { $fields[$index] } else { '' }
    return [pscustomobject]@{ Status = 'read'; Value = $value }
}

<#
.SYNOPSIS
    One invocation-log line exactly as this build writes it: the version and every column of the
    layout, -Outcome and -Source in theirs and every other column empty. What a stand-in launcher
    records, so a reader is tested against a line the launcher's own reader would read. The
    POSIX twin is `e2e_launcher_log_line`.
#>
function New-E2ELauncherLogLine([string]$Outcome, [string]$Source) {
    $layout = Get-E2ELauncherLogLayout
    $columns = foreach ($column in $layout.Columns) {
        if ($column -eq 'outcome') { $Outcome } elseif ($column -eq 'source') { $Source } else { '' }
    }
    return (@($layout.Version) + @($columns)) -join "`t"
}

<#
.SYNOPSIS
    Column -Name of one invocation-log line, by NAME (`Get-E2ELauncherLogLayout`).
.DESCRIPTION
    Throws for a column the log does not have, for a line naming a version this build does not
    write and for a line in this build's version without its columns (`Read-E2ELauncherLogLine`):
    read by position, such a line is misread. The POSIX twin is `e2e_launcher_log_field`.
#>
function Get-E2ELauncherLogField([Parameter(Mandatory)][AllowEmptyString()][string]$Line, [Parameter(Mandatory)][string]$Name) {
    if ($null -eq $script:LauncherLogLayout) { $script:LauncherLogLayout = Get-E2ELauncherLogLayout }
    $read = Read-E2ELauncherLogLine $Line $Name $script:LauncherLogLayout
    switch ($read.Status) {
        'foreign' { throw "an invocation-log line names version $(($Line -split "`t")[0]), and this build writes $($script:LauncherLogLayout.Version)" }
        'malformed' { throw "an invocation-log $($script:LauncherLogLayout.Version) line does not carry this build's $($script:LauncherLogLayout.Columns.Count) column(s)" }
    }
    return $read.Value
}

<#
.SYNOPSIS
    The platform whose branch of `StateDirectoryImpl` applies to this process: `windows`
    or `posix`.
#>
function Get-E2ELauncherStatePlatform {
    if ($env:OS -eq 'Windows_NT') { return 'windows' }
    return 'posix'
}

<#
.SYNOPSIS
    The environment variables the launcher's state directory is resolved from ON ONE
    PLATFORM, read out of `StateDirectoryImpl`, one row per variable in the order the source
    reads them: its Name, and the BaseSuffix the launcher appends when that variable is the
    base (`/.local/state` for HOME).
.PARAMETER Platform
    `windows` or `posix`; this process's by default. Only that branch of
    `#if defined(_WIN32)` / `#else` / `#endif` is read: redirecting a variable the launcher
    does not read there is not harmless, since its children -- `cl`, `clang-cl` -- inherit it.
.DESCRIPTION
    Throws rather than guessing: a body it cannot find, any other preprocessor line, a
    variable read that is never a base, or a base built from something that is not a
    variable read. A partial answer is the one that leaks. What this reader cannot see at
    all -- a base from a helper, or from `std::getenv` -- is caught by the read-only guard
    in `Enter-E2ELauncherState`, which asks the launcher itself.
#>
function Get-E2ELauncherStateRows([string]$Path = $script:LauncherStateSource, [string]$Platform = (Get-E2ELauncherStatePlatform)) {
    if ($Platform -notin 'windows', 'posix') { throw "unknown platform '$Platform': windows or posix" }
    $text = [IO.File]::ReadAllText($Path)
    $body = [regex]::Match($text, '(?s)StateDirectoryImpl\(\)\s*\{(.*?)\r?\n    \}')
    if (-not $body.Success) { throw "no StateDirectoryImpl() body in $Path" }
    $kept = New-Object System.Collections.Generic.List[string]
    $branch = ''
    $shape = "StateDirectoryImpl in $Path has a preprocessor line this reader does not understand; it reads #if defined(_WIN32), #else and #endif only"
    foreach ($line in ($body.Groups[1].Value -split "`n")) {
        $line = $line.TrimEnd("`r")
        if ($line -match '^\s*#\s*if\s+defined\s*\(_WIN32\)\s*$') { if ($branch) { throw $shape }; $branch = 'windows'; continue }
        if ($line -match '^\s*#\s*else\s*$') { if ($branch -ne 'windows') { throw $shape }; $branch = 'posix'; continue }
        if ($line -match '^\s*#\s*endif\s*$') { if (-not $branch) { throw $shape }; $branch = ''; continue }
        if ($line -match '^\s*#') { throw $shape }
        if (-not $branch -or $branch -eq $Platform) { $kept.Add($line) }
    }
    if ($branch) { throw $shape }
    $source = $kept -join "`n"
    $reads = [regex]::Matches($source, '(\w+) = FastCache::ReadEnvironmentVariable\("([A-Z_]+)"\)')
    $bases = [regex]::Matches($source, 'base = \*(\w+)(?:\s*\+\s*"([^"]*)")?\s*;')
    $rows = @()
    foreach ($read in $reads) {
        $base = @($bases | Where-Object { $_.Groups[1].Value -eq $read.Groups[1].Value })
        if ($base.Count -ne 1) { throw "StateDirectoryImpl reads $($read.Groups[2].Value) but uses it as a base $($base.Count) times, not once" }
        $rows += [pscustomobject]@{ Name = $read.Groups[2].Value; BaseSuffix = $base[0].Groups[2].Value }
    }
    foreach ($base in $bases) {
        if (-not @($reads | Where-Object { $_.Groups[1].Value -eq $base.Groups[1].Value }).Count) {
            throw "StateDirectoryImpl builds its base from '$($base.Groups[1].Value)', which is no environment read this reader recognises"
        }
    }
    if ($rows.Count -eq 0) { throw "StateDirectoryImpl in $Path reads no environment variable this reader recognises on $Platform" }
    return $rows
}

# The commands a `$Launcher` reference may sit in without running the launcher, as the
# NEAREST enclosing command. `Returns` says whether the command hands back a PATH derived
# from it: for those the exemption covers only a result that ends at a known sink -- see
# `Test-E2ELauncherResultReachesSink`. Each row says why.
$script:LauncherReferenceExemptions = @{
    'Test-Path'              = @{ Returns = $false; Why = 'asks whether the binary exists, and answers a boolean' }
    'Write-Host'             = @{ Returns = $false; Why = 'names the path in a message, and returns nothing' }
    'Enter-E2ELauncherState' = @{ Returns = $false; Why = 'hands the path to this seam, whose one launcher run is the read-only guard, inside the wrapper' }
    'Resolve-Path'           = @{ Returns = $true;  Why = 'spells the path absolutely' }
    'Split-Path'             = @{ Returns = $true;  Why = 'derives a directory from the path' }
    'Join-Path'              = @{ Returns = $true;  Why = 'derives a path' }
}

# The AST nodes a derived path passes through unchanged on its way to wherever it is used.
# A pipeline and a method call are NOT here: each passes the path on only from one position,
# which `Test-E2ELauncherResultReachesSink` asks about by name.
$script:LauncherResultCarriers = @(
    [System.Management.Automation.Language.CommandExpressionAst],
    [System.Management.Automation.Language.ParenExpressionAst], [System.Management.Automation.Language.SubExpressionAst],
    [System.Management.Automation.Language.StatementBlockAst], [System.Management.Automation.Language.MemberExpressionAst],
    [System.Management.Automation.Language.ArrayLiteralAst], [System.Management.Automation.Language.ArrayExpressionAst],
    [System.Management.Automation.Language.ExpandableStringExpressionAst], [System.Management.Automation.Language.BinaryExpressionAst],
    [System.Management.Automation.Language.ConvertExpressionAst], [System.Management.Automation.Language.IndexExpressionAst],
    [System.Management.Automation.Language.CommandParameterAst])

<#
.SYNOPSIS
    A variable's name without its scope: `$script:Launcher`, `$global:Launcher` and
    `$Launcher` are one variable to this scan. `VariablePath.UnqualifiedPath` is not public
    in every PowerShell, so the prefix is stripped from `UserPath`.
#>
function Get-E2ELauncherVariableName([System.Management.Automation.Language.VariableExpressionAst]$Variable) {
    return $Variable.VariablePath.UserPath -replace '^[A-Za-z]+:', ''
}

<#
.SYNOPSIS
    Whether @Node reads the launcher parameter by NAME through the bound-parameter table --
    `$PSBoundParameters["Launcher"]`, `$PSBoundParameters.Launcher`, or the same through
    `$MyInvocation.BoundParameters` -- which is the idiomatic spelling that never writes
    `$Launcher`, and so a reference to it like any other.
#>
function Test-E2ELauncherBoundParameter([System.Management.Automation.Language.Ast]$Node) {
    $table = $null
    if ($Node -is [System.Management.Automation.Language.IndexExpressionAst] -and
        $Node.Index -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $Node.Index.Value -eq 'Launcher') {
        $table = $Node.Target
    }
    elseif ($Node -is [System.Management.Automation.Language.MemberExpressionAst] -and
            $Node -isnot [System.Management.Automation.Language.InvokeMemberExpressionAst] -and
            $Node.Member -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $Node.Member.Value -eq 'Launcher') {
        $table = $Node.Expression
    }
    if (-not $table) { return $false }
    if ($table -is [System.Management.Automation.Language.VariableExpressionAst]) {
        return (Get-E2ELauncherVariableName $table) -eq 'PSBoundParameters'
    }
    return $table -is [System.Management.Automation.Language.MemberExpressionAst] -and
           $table.Member -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
           $table.Member.Value -eq 'BoundParameters'
}

<#
.SYNOPSIS
    Whether the path @Command returns ends at a KNOWN sink -- an assignment, or an exempt
    command that returns no path (`Test-Path`, `Write-Host`, `Enter-E2ELauncherState`) --
    followed through the parentheses, members and strings it travels in and through another
    path-returning exemption (`Join-Path (Split-Path $Launcher) ...`).
.DESCRIPTION
    Fails CLOSED: every other consumer is a refusal -- the target of `&` or `.`, a later
    pipeline stage, a method's argument, any other command's argument (`Get-Item`,
    `cmd /c`, an executor), a hashtable, a statement's output. An ASSIGNMENT is accepted
    and ends the walk unjudged, because what the variable is used for next is not this
    walk's to see: that is the scan's stated blind spot, `$p = (Resolve-Path $Launcher).Path`
    and later `& $p`.
#>
function Test-E2ELauncherResultReachesSink([System.Management.Automation.Language.CommandAst]$Command) {
    $node = $Command
    while ($node.Parent) {
        $parent = $node.Parent
        if ($parent -is [System.Management.Automation.Language.PipelineAst]) {
            # Only the LAST stage's output leaves the pipeline; before it, the next stage consumes it.
            if (-not [object]::ReferenceEquals($parent.PipelineElements[$parent.PipelineElements.Count - 1], $node)) { return $false }
            $node = $parent; continue
        }
        if ($parent -is [System.Management.Automation.Language.InvokeMemberExpressionAst]) {
            # `(...).ToString()` carries the path on; `[Process]::Start((...))` consumes it.
            if ([object]::ReferenceEquals($parent.Expression, $node)) { $node = $parent; continue }
            return $false
        }
        if (@($script:LauncherResultCarriers | Where-Object { $parent -is $_ }).Count) { $node = $parent; continue }
        if ($parent -is [System.Management.Automation.Language.AssignmentStatementAst]) { return $true }
        if ($parent -is [System.Management.Automation.Language.CommandAst]) {
            if ([object]::ReferenceEquals($parent.CommandElements[0], $node)) { return $false }
            $row = $script:LauncherReferenceExemptions[[string]$parent.GetCommandName()]
            if ($row -and $row.Returns) { $node = $parent; continue }
            return [bool]$row
        }
        return $false
    }
    return $false
}

<#
.SYNOPSIS
    Whether a `Start-Job` hands @Reference to a script block that binds it to a parameter
    named `Launcher` -- the one shape in which the job's own launcher run is judged by this
    same scan, as `$Launcher` inside the block. Anything else, or anything this cannot
    read, is a refusal.
#>
function Test-E2ELauncherJobBinding([System.Management.Automation.Language.CommandAst]$Job, $Reference) {
    $elements = $Job.CommandElements
    if ($elements.Count -lt 2) { return $false }
    $arguments = $null; $block = $null
    foreach ($i in 1..($elements.Count - 1)) {
        $element = $elements[$i]
        if ($element -isnot [System.Management.Automation.Language.CommandParameterAst]) { continue }
        $value = if ($element.Argument) { $element.Argument } elseif ($i + 1 -lt $elements.Count) { $elements[$i + 1] } else { $null }
        $parameter = $element.ParameterName
        if ($parameter -eq 'Args' -or ($parameter.Length -ge 3 -and 'ArgumentList'.StartsWith($parameter, [StringComparison]::OrdinalIgnoreCase))) { $arguments = $value }
        if ($parameter.Length -ge 2 -and 'ScriptBlock'.StartsWith($parameter, [StringComparison]::OrdinalIgnoreCase)) { $block = $value }
    }
    # `Start-Job { ... } -ArgumentList ...`: the block given positionally.
    if (-not $block) { $block = @($elements | Where-Object { $_ -is [System.Management.Automation.Language.ScriptBlockExpressionAst] }) | Select-Object -First 1 }
    if (-not $arguments -or -not $block) { return $false }
    $position = if ([object]::ReferenceEquals($arguments, $Reference)) { 0 }
                elseif ($arguments -is [System.Management.Automation.Language.ArrayLiteralAst]) {
                    $at = -1
                    foreach ($j in 0..($arguments.Elements.Count - 1)) { if ([object]::ReferenceEquals($arguments.Elements[$j], $Reference)) { $at = $j } }
                    $at
                } else { -1 }
    if ($position -lt 0) { return $false }
    if ($block -is [System.Management.Automation.Language.VariableExpressionAst]) {
        $root = $Job; while ($root.Parent) { $root = $root.Parent }
        $name = $block.VariablePath.UserPath
        $assigned = @($root.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and
                    $n.Left -is [System.Management.Automation.Language.VariableExpressionAst] -and $n.Left.VariablePath.UserPath -eq $name }, $true))
        if ($assigned.Count -ne 1) { return $false }
        $block = $assigned[0].Right
        while ($block -and $block -isnot [System.Management.Automation.Language.ScriptBlockExpressionAst]) {
            $block = if ($block -is [System.Management.Automation.Language.PipelineAst] -and $block.PipelineElements.Count -eq 1) { $block.PipelineElements[0] }
                     elseif ($block -is [System.Management.Automation.Language.CommandExpressionAst]) { $block.Expression }
                     else { $null }
        }
    }
    if ($block -isnot [System.Management.Automation.Language.ScriptBlockExpressionAst]) { return $false }
    $parameters = $block.ScriptBlock.ParamBlock.Parameters
    if (-not $parameters -or $parameters.Count -le $position) { return $false }
    return (Get-E2ELauncherVariableName $parameters[$position].Name) -eq 'Launcher'
}

<#
.SYNOPSIS
    Every `$Launcher` reference in @Tree -- `$script:`, `$global:`, `$using:` and `${}`
    spellings included -- that could run the launcher outside `Use-E2ELauncherState`.
.DESCRIPTION
    A reference is accepted when it declares the parameter, writes the variable, is the
    operand of `-not`, sits inside a `Use-E2ELauncherState` block, is handed by `Start-Job`
    to a block parameter named `Launcher`, or its NEAREST enclosing command is an exemption
    that does not run it -- only the nearest, so `Write-Host "$(& $Launcher -z)"` is not
    laundered by the outer command -- and, for a command that returns a derived path, whose
    result ends at a KNOWN sink (`Test-E2ELauncherResultReachesSink`); any other consumer of
    it -- a pipeline stage, a method, `Get-Item`, `cmd /c`, a hashtable -- is refused. Every
    other reference is refused, which makes an alias (`$cc = $Launcher`) a refusal as well
    as a bare call.

    Blind spots, each failing OPEN, and caught at run time by the positive control and the
    caller check instead:
      * a launcher reached without naming `$Launcher` at all -- a second copy of the path
        taken from the command line, `Get-Variable -Name ("Laun" + "cher") -ValueOnly`, or
        `$ExecutionContext.InvokeCommand.ExpandString('$Launcher')`. The bound parameter
        IS counted, as `$PSBoundParameters["Launcher"]`, `$PSBoundParameters.Launcher` and
        `$MyInvocation.BoundParameters.Launcher` (`Test-E2ELauncherBoundParameter`);
      * a path an exempt command derives from `$Launcher` and the fixture STORES in another
        variable, then runs through that variable (`$d = Split-Path $Launcher` and later
        `& (Join-Path $d fastcache-cc.exe)`): an assignment is the one sink accepted without
        following what the variable is used for next.
.OUTPUTS
    References (how many were judged) and Violations (one line each).
#>
function Get-E2EUnwrappedLauncherUse([System.Management.Automation.Language.Ast]$Tree) {
    $refs = $Tree.FindAll({ param($n) ($n -is [System.Management.Automation.Language.VariableExpressionAst] -and
            (Get-E2ELauncherVariableName $n) -eq 'Launcher') -or (Test-E2ELauncherBoundParameter $n) }, $true)
    $violations = @()
    foreach ($ref in $refs) {
        $declares = $ref.Parent -is [System.Management.Automation.Language.ParameterAst]
        $writes = $ref.Parent -is [System.Management.Automation.Language.AssignmentStatementAst] -and $ref.Parent.Left -eq $ref
        # `-not $Launcher` is a truth test: its value is a boolean, never the path, so it
        # can neither run the launcher nor alias it. Only that exact shape -- any other
        # expression could carry the path onward.
        $predicate = $ref.Parent -is [System.Management.Automation.Language.UnaryExpressionAst] -and
                     $ref.Parent.TokenKind -eq [System.Management.Automation.Language.TokenKind]::Not
        $wrapped = $false
        $node = $ref.Parent
        while ($node -and -not $wrapped) {
            $wrapped = $node -is [System.Management.Automation.Language.ScriptBlockExpressionAst] -and
                       $node.Parent -is [System.Management.Automation.Language.CommandAst] -and
                       $node.Parent.GetCommandName() -eq $script:LauncherStateWrapper
            $node = $node.Parent
        }
        $nearest = $ref.Parent
        while ($nearest -and $nearest -isnot [System.Management.Automation.Language.CommandAst]) { $nearest = $nearest.Parent }
        $exempt = $false
        if ($nearest -and -not ($declares -or $writes -or $predicate -or $wrapped)) {
            $target = $nearest.CommandElements[0]
            $runsIt = [object]::ReferenceEquals($target, $ref) -or
                      ($ref.Extent.StartOffset -ge $target.Extent.StartOffset -and $ref.Extent.EndOffset -le $target.Extent.EndOffset)
            $name = [string]$nearest.GetCommandName()
            $row = $script:LauncherReferenceExemptions[$name]
            $exempt = if ($runsIt) { $false }
                      elseif ($name -in 'Start-Job', 'sajb') { Test-E2ELauncherJobBinding $nearest $ref }
                      elseif ($row) { (-not $row.Returns) -or (Test-E2ELauncherResultReachesSink $nearest) }
                      else { $false }
        }
        if (-not ($declares -or $writes -or $predicate -or $exempt -or $wrapped)) {
            $violations += "line $($ref.Extent.StartLineNumber): $($ref.Parent.Extent.Text.Split("`n")[0].Trim())"
        }
    }
    return [pscustomobject]@{ References = $refs.Count; Violations = $violations }
}

# The scan's control rows: planted text, how many `$Launcher` references it holds, and how
# many of them must be refused. Every laundering shape a review found is a row, refused.
$script:LauncherScanControls = @(
    @{ Text = 'Use-E2ELauncherState $h { & $Launcher -s }'; References = 1; Refused = 0 }
    @{ Text = 'Test-Path $Launcher'; References = 1; Refused = 0 }
    @{ Text = '$Launcher = (Resolve-Path $Launcher).Path'; References = 2; Refused = 0 }
    @{ Text = 'if (-not $Launcher) { }'; References = 1; Refused = 0 }
    @{ Text = 'Write-Host "launcher: $Launcher"'; References = 1; Refused = 0 }
    @{ Text = '$scratch = Join-Path (Split-Path (Split-Path $Launcher -Parent) -Parent) "scratch"'; References = 1; Refused = 0 }
    @{ Text = 'Enter-E2ELauncherState -Launcher $Launcher -Fixture $f'; References = 1; Refused = 0 }
    @{ Text = 'Start-Job -ScriptBlock { param($Launcher) Use-E2ELauncherState $h { & $Launcher } } -ArgumentList $Launcher'; References = 3; Refused = 0 }
    @{ Text = "`$body = { param(`$x, `$Launcher) Use-E2ELauncherState `$h { & `$Launcher } }`nStart-Job -ScriptBlock `$body -ArgumentList 1,`$Launcher"; References = 3; Refused = 0 }
    @{ Text = '& $Launcher -z'; References = 1; Refused = 1 }
    @{ Text = '$cc = $Launcher'; References = 1; Refused = 1 }
    @{ Text = 'Write-Host "$(& $Launcher -z)"'; References = 1; Refused = 1 }
    @{ Text = 'Start-Process -FilePath $Launcher'; References = 1; Refused = 1 }
    @{ Text = '& (Resolve-Path $Launcher).Path -z'; References = 1; Refused = 1 }
    @{ Text = 'Start-Process -FilePath (Resolve-Path $Launcher) -ArgumentList "-z" -Wait'; References = 1; Refused = 1 }
    @{ Text = '& (Join-Path (Split-Path $Launcher) "fastcache-cc.exe") -z'; References = 1; Refused = 1 }
    @{ Text = 'Start-Job -ScriptBlock { param($l) & $l -z } -ArgumentList $Launcher'; References = 1; Refused = 1 }
    @{ Text = '& $script:Launcher -z'; References = 1; Refused = 1 }
    @{ Text = '& $global:Launcher -z'; References = 1; Refused = 1 }
    @{ Text = 'Start-Job -ScriptBlock { & $using:Launcher -z }'; References = 1; Refused = 1 }
    @{ Text = '. (Resolve-Path $Launcher) -z'; References = 1; Refused = 1 }
    @{ Text = '& "$(Split-Path $Launcher)\fastcache-cc.exe" -z'; References = 1; Refused = 1 }
    @{ Text = 'Invoke-Command -ScriptBlock { param($p) & $p } -ArgumentList (Resolve-Path $Launcher)'; References = 1; Refused = 1 }
    @{ Text = 'Resolve-Path $Launcher | ForEach-Object { & $_ -z }'; References = 1; Refused = 1 }
    @{ Text = '& (Get-Item (Resolve-Path $Launcher)) -z'; References = 1; Refused = 1 }
    @{ Text = '[Diagnostics.Process]::Start((Resolve-Path $Launcher).Path, "-z")'; References = 1; Refused = 1 }
    @{ Text = 'cmd /c "$(Resolve-Path $Launcher) -z"'; References = 1; Refused = 1 }
    @{ Text = '$a = @{ FilePath = (Resolve-Path $Launcher) }; Start-Process @a'; References = 1; Refused = 1 }
    # The same two consumers ending in an ASSIGNMENT, which is a sink: only the pipeline and
    # method rules refuse these, where the shapes above would be refused without them.
    @{ Text = '$x = Resolve-Path $Launcher | ForEach-Object { & $_ -z }'; References = 1; Refused = 1 }
    @{ Text = '$p = [Diagnostics.Process]::Start((Resolve-Path $Launcher).Path, "-z")'; References = 1; Refused = 1 }
    # The parameter read by NAME from the bound-parameter table, never writing `$Launcher`.
    @{ Text = '& $PSBoundParameters["Launcher"] -z'; References = 1; Refused = 1 }
    @{ Text = '& $PSBoundParameters.Launcher -z'; References = 1; Refused = 1 }
    @{ Text = '& $MyInvocation.BoundParameters.Launcher -z'; References = 1; Refused = 1 }
    @{ Text = 'Use-E2ELauncherState $h { & $PSBoundParameters["Launcher"] -s }'; References = 1; Refused = 0 }
    @{ Text = 'if ($PSBoundParameters.ContainsKey("Launcher")) { }'; References = 0; Refused = 0 }
)

<#
.SYNOPSIS
    The scan's positive control: every row of `$LauncherScanControls` judged as planted.
    Returns an empty string when the scan behaves, else why it cannot be trusted.
#>
function Test-E2EUnwrappedLauncherScan {
    $wrong = @()
    foreach ($row in $script:LauncherScanControls) {
        $found = Get-E2EUnwrappedLauncherUse ([System.Management.Automation.Language.Parser]::ParseInput($row.Text, [ref]$null, [ref]$null))
        if ($found.References -ne $row.References -or $found.Violations.Count -ne $row.Refused) {
            $wrong += "'$($row.Text)': judged $($found.References) and refused $($found.Violations.Count), must judge $($row.References) and refuse $($row.Refused)"
        }
    }
    if ($wrong.Count) { return "the unwrapped-launcher scan misjudges $($wrong.Count) of its $($script:LauncherScanControls.Count) control row(s): " + ($wrong -join '; ') }
    return ""
}

<#
.SYNOPSIS
    One snapshot of the log at @Path: whether it exists, its size, and a SHA-256 over its
    last `LauncherStateTailBytes` bytes -- or over the bytes before @Size when that is given.
#>
function Get-E2ELauncherLogTailHash([string]$Path, [long]$Size) {
    $stream = [IO.File]::Open($Path, 'Open', 'Read', [IO.FileShare]'ReadWrite, Delete')
    try {
        $from = [Math]::Max([long]0, $Size - $script:LauncherStateTailBytes)
        $count = [int]($Size - $from)
        $bytes = New-Object byte[] $count
        $null = $stream.Seek($from, 'Begin')
        $read = 0
        while ($read -lt $count) {
            $n = $stream.Read($bytes, $read, $count - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha.ComputeHash($bytes, 0, $read)) } finally { $sha.Dispose() }
    }
    finally { $stream.Dispose() }
}

<#
.SYNOPSIS
    Snapshot every log the caller's environment would have the launcher write, one per state
    variable that is set on this platform: Path, Present, Size, Tail.
#>
function Get-E2ELauncherCallerSnapshot {
    $seen = @{}
    foreach ($row in @(Get-E2ELauncherStateRows)) {
        $value = [Environment]::GetEnvironmentVariable($row.Name)
        if (-not $value) { continue }
        $log = Join-Path ($value.TrimEnd('\', '/') + $row.BaseSuffix) $script:LauncherStateLogRelative
        if ($seen.ContainsKey($log)) { continue }
        $seen[$log] = $true
        $item = Get-Item -LiteralPath $log -ErrorAction SilentlyContinue
        if ($item) {
            [pscustomobject]@{ Path = $log; Present = $true; Size = $item.Length; Tail = (Get-E2ELauncherLogTailHash $log $item.Length) }
        } else {
            [pscustomobject]@{ Path = $log; Present = $false; Size = 0; Tail = '' }
        }
    }
}

<#
.SYNOPSIS
    Refuse a fixture any of whose launcher executions sits outside `Use-E2ELauncherState`.
    Throws naming each one; returns nothing when the fixture is clean.
.PARAMETER Fixture
    The fixture script, read as a syntax tree -- never run.
.DESCRIPTION
    A fixture calls this at STARTUP, before its skips and its self-test modes, so a machine
    that skips the fixture -- no compiler, no built binary -- still judges it; and
    `Enter-E2ELauncherState` calls it again, so a fixture that enters the seam is held to it
    whatever it did first. The scan's own control rows run first: a scan that refuses
    nothing is not evidence that the fixture is clean. A fixture with no `$Launcher`
    reference at all is refused as well, because a renamed variable would read as clean.

    The FIRST call also snapshots the caller's logs, before anything in the fixture can
    have run a launcher; the end-of-run check judges against that picture.
#>
function Assert-E2ELauncherFixture([Parameter(Mandatory)][string]$Fixture) {
    if ($null -eq $script:LauncherCallerSnapshot) { $script:LauncherCallerSnapshot = @(Get-E2ELauncherCallerSnapshot) }
    $scanBroken = Test-E2EUnwrappedLauncherScan
    if ($scanBroken) { throw "$scanBroken; refusing to trust it" }
    $scan = Get-E2EUnwrappedLauncherUse ([System.Management.Automation.Language.Parser]::ParseFile($Fixture, [ref]$null, [ref]$null))
    if ($scan.References -eq 0) { throw "found no `$Launcher reference in $Fixture to judge; a renamed variable must not read as clean" }
    if ($scan.Violations.Count -gt 0) {
        throw ("a launcher runs outside $($script:LauncherStateWrapper) in $Fixture, where it would read or delete the caller's statistics:`n  " +
               ($scan.Violations -join "`n  "))
    }
}

$script:LauncherStateEntries = 0

<#
.SYNOPSIS
    Give a fixture's launcher a state directory of the run's own. Returns the handle every
    other launcher-state function takes; throws when the fixture cannot be isolated.
.PARAMETER Launcher
    The launcher the fixture runs.
.PARAMETER Fixture
    The fixture script, whose every launcher execution must sit inside
    `Use-E2ELauncherState` -- scanned here, so a fixture that enters the seam is held to it.
.PARAMETER RunTrees
    Directories this run's sources live under: a caller record naming one is this run's.
.DESCRIPTION
    Before returning it asks the LAUNCHER where it will record, through the wrapper:
    `--show-stats` names the log only while the log is absent or empty, which a fresh root
    always is, so the question is read-only -- `-z` names it too, but only by deleting what
    is there, which would destroy the caller's log on exactly the failure being checked.
    Refused both ways a redirect can miss: a path outside the root, or a report naming no
    path. Call it INSIDE the `try` whose `finally` calls `Exit-E2ELauncherState`.
#>
function Enter-E2ELauncherState([Parameter(Mandatory)][string]$Launcher, [Parameter(Mandatory)][string]$Fixture, [string[]]$RunTrees = @()) {
    Assert-E2ELauncherFixture -Fixture $Fixture

    $rows = @(Get-E2ELauncherStateRows)
    $script:LauncherStateEntries++
    $root = Join-Path ([IO.Path]::GetTempPath()) "fastcache-cc-e2e-state-$PID-$($script:LauncherStateEntries)"
    # Safe to clear ONLY because the name carries this process's id: what it can reach is
    # this process's own leftover, or a dead one's -- `src/tests/ScratchPath.hpp`'s argument.
    Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $root | Out-Null
    $handle = [pscustomobject]@{
        Launcher = $Launcher; Rows = $rows; Variables = @($rows | ForEach-Object Name)
        RunRoot = $root; Root = $root; Log = ""; RunTrees = @($RunTrees | Where-Object { $_ })
        CallerSnapshot = @($script:LauncherCallerSnapshot)
    }

    $shown = Use-E2ELauncherState $handle { & $handle.Launcher --show-stats 2>&1 | Out-String }
    $named = [regex]::Match($shown, 'no statistics recorded yet \((.+?)\)\.')
    $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $named.Success -or -not $named.Groups[1].Value.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        $null = Exit-E2ELauncherState $handle
        throw "the launcher would not record under this run's state root ($root) -- the redirect misses a variable it reads; it printed:`n$shown"
    }
    $handle.Log = $named.Groups[1].Value
    $relative = $handle.Log.Substring($prefix.Length) -replace '\\', '/'
    if ($relative -ne $script:LauncherStateLogRelative) {
        $null = Exit-E2ELauncherState $handle
        throw "the launcher keeps its log at $relative under its state directory, but the caller's were snapshotted at $($script:LauncherStateLogRelative); update LauncherStateLogRelative"
    }
    return $handle
}

<#
.SYNOPSIS
    Run @Run -- which starts the launcher -- with every state variable pointing at
    @Handle.Root, and put the caller's values back however it ends: a variable that was
    UNSET is unset again, not left empty.
.DESCRIPTION
    What @Run writes to the pipeline is what this returns; `$LASTEXITCODE` is global and
    survives. The launcher's own children (`cl`, `clang-cl`) inherit the redirect, which is
    why only the variables the launcher reads on this platform are redirected. A case that
    must count its own records points @Handle.Root somewhere of its own for the length of
    the case. The block runs in a child of THIS scope, so this function's locals carry names
    no call site uses.
#>
function Use-E2ELauncherState([Parameter(Mandatory)]$Handle, [Parameter(Mandatory)][scriptblock]$Run) {
    $launcherStateSaved = @{}
    foreach ($launcherStateName in $Handle.Variables) {
        $launcherStateSaved[$launcherStateName] = [Environment]::GetEnvironmentVariable($launcherStateName)
        [Environment]::SetEnvironmentVariable($launcherStateName, $Handle.Root)
    }
    try { & $Run }
    finally {
        foreach ($launcherStateName in $Handle.Variables) {
            if ($null -eq $launcherStateSaved[$launcherStateName]) {
                Remove-Item -LiteralPath "Env:\$launcherStateName" -ErrorAction SilentlyContinue
            } else {
                [Environment]::SetEnvironmentVariable($launcherStateName, $launcherStateSaved[$launcherStateName])
            }
        }
    }
}

<#
.SYNOPSIS
    The positive control: the run's own log holds records. Returns how many.
.DESCRIPTION
    That the caller's log was spared is no evidence the launcher recorded HERE -- a
    launcher recording nowhere spares it too. Ask it BEFORE any `-z`, which clears it.
#>
function Get-E2ELauncherStateRecordCount([Parameter(Mandatory)]$Handle) {
    return @(Get-Content $Handle.Log -ErrorAction SilentlyContinue).Count
}

<#
.SYNOPSIS
    What this run did to the caller's logs since the fixture's startup: one line per finding,
    none when it did nothing.
.DESCRIPTION
    A log present at startup keeps every byte it had -- it is not deleted, not shorter, and
    its tail hashes the same -- and gains no record naming one of this run's trees. A log
    ABSENT at startup, the ordinary state of every CI runner, is still absent or holds no
    record of this run: other builds on the machine may create it meanwhile, and are told
    apart by the source each record names. Blind spot, failing OPEN: a record whose source
    is relative, or spelled through a `subst` drive or an 8.3 name, names no run tree.
#>
function Get-E2ELauncherCallerDamage([Parameter(Mandatory)]$Handle, [string[]]$RunTrees = @()) {
    $trees = @(@($RunTrees) + @($Handle.RunTrees) + @($Handle.RunRoot)) | Where-Object { $_ }
    $damage = @()
    # Read ONCE, and a layout that cannot be read is its own answer -- not "a foreign version",
    # which would send the reader after the wrong cause.
    $layout = $null
    $layoutFailure = $null
    try { $layout = Get-E2ELauncherLogLayout } catch { $layoutFailure = $_.Exception.Message }
    foreach ($was in @($Handle.CallerSnapshot)) {
        $item = Get-Item -LiteralPath $was.Path -ErrorAction SilentlyContinue
        $from = [long]0
        if ($was.Present) {
            if (-not $item) { $damage += "the caller's statistics log was DELETED during this run ($($was.Path), $($was.Size) byte(s) at startup)"; continue }
            if ($item.Length -lt $was.Size) { $damage += "the caller's statistics log lost records during this run ($($was.Path): $($was.Size) byte(s) at startup, $($item.Length) now)"; continue }
            if ((Get-E2ELauncherLogTailHash $was.Path $was.Size) -ne $was.Tail) { $damage += "the caller's statistics log was rewritten during this run ($($was.Path): its first $($was.Size) byte(s) changed)"; continue }
            $from = $was.Size
        } elseif (-not $item) { continue }
        $stream = [IO.File]::Open($was.Path, 'Open', 'Read', [IO.FileShare]'ReadWrite, Delete')
        try {
            $null = $stream.Seek($from, 'Begin')
            $appended = (New-Object IO.StreamReader($stream)).ReadToEnd()
        } finally { $stream.Dispose() }
        if ($null -eq $layout) {
            $damage += "the launcher's log layout could not be read ($layoutFailure), so nothing can say which records in the caller's statistics log are this run's ($($was.Path))"
            continue
        }
        # The source by its column NAME. A record in a version this build does not write, one
        # without this build's columns, and a line the reader failed on are three answers, each
        # reported: nothing can say whose such a record is.
        $ours = 0
        $tally = @{ foreign = 0; malformed = 0; failed = 0 }
        $failure = ''
        foreach ($line in @($appended -split "`r?`n")) {
            if (-not $line) { continue }
            try { $read = Read-E2ELauncherLogLine $line 'source' $layout } catch { $tally.failed++; $failure = $_.Exception.Message; continue }
            if ($read.Status -ne 'read') { $tally[$read.Status]++; continue }
            $source = $read.Value
            if ($source -and @($trees | Where-Object { $source.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0) { $ours++ }
        }
        if ($ours -gt 0) { $damage += "$ours of this run's compiles were recorded in the caller's statistics log ($($was.Path))" }
        if ($tally.foreign -gt 0) { $damage += "$($tally.foreign) record(s) appended to the caller's statistics log during this run name a log version this build does not write ($($was.Path)), so nothing can say whether they are this run's" }
        if ($tally.malformed -gt 0) { $damage += "$($tally.malformed) record(s) appended to the caller's statistics log during this run do not carry this build's columns ($($was.Path)), so nothing can say whether they are this run's" }
        if ($tally.failed -gt 0) { $damage += "$($tally.failed) record(s) appended to the caller's statistics log during this run could not be read ($failure; $($was.Path)), so nothing can say whether they are this run's" }
    }
    return $damage
}

<#
.SYNOPSIS
    End the run's use of the seam: remove its state root, and return what the run did to the
    caller's logs (see `Get-E2ELauncherCallerDamage`) -- one line per finding.
.DESCRIPTION
    Called from the fixture's `finally`, so a fixture that fails midway still reports the
    damage it did: a leak is likeliest exactly on a failure path. The caller's variables
    need no restoring here; the wrapper restored them after every launcher it ran.
#>
function Exit-E2ELauncherState($Handle, [string[]]$RunTrees = @()) {
    if (-not $Handle) { return }
    if ($Handle.RunRoot) { Remove-Item -Recurse -Force $Handle.RunRoot -ErrorAction SilentlyContinue }
    if ($null -ne $Handle.CallerSnapshot) { Get-E2ELauncherCallerDamage $Handle -RunTrees $RunTrees }
}

<#
.SYNOPSIS
    Drive the seam end to end against a stand-in launcher and a stand-in caller, with the
    caller's log PRESENT, EMPTY and ABSENT. Returns one line per failure; prints the count.
.DESCRIPTION
    The stand-in launcher is generated from this platform's rows and honours them in the
    order the source reads them, the first one set winning -- `StateDirectoryImpl`'s
    precedence. It answers `--show-stats`, `-z` and a compile, which appends a record naming
    its last argument. Every case runs with every state variable pointing at a caller
    directory of its own, and this process's environment is put back afterwards.
#>
function Test-E2ELauncherStateSeam {
    $failures = @()
    $cases = 0
    $work = Join-Path ([IO.Path]::GetTempPath()) "fastcache-cc-seam-selftest-$PID"
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $work | Out-Null
    $rows = @(Get-E2ELauncherStateRows)
    # The stand-in records in the CURRENT version, as the launcher does: one writing an older
    # layout is one the caller-damage check can read while it misreads the real launcher.
    $logVersion = (Get-E2ELauncherLogLayout).Version
    $logLine = New-E2ELauncherLogLine 'MISS' '@SOURCE@'
    $fake = Join-Path $work 'fake-launcher.ps1'
    $pick = ($rows | ForEach-Object { "if (-not `$base -and [Environment]::GetEnvironmentVariable('$($_.Name)')) { `$base = [Environment]::GetEnvironmentVariable('$($_.Name)') + '$($_.BaseSuffix)' }" }) -join "`n"
    Set-Content -LiteralPath $fake -Value @"
`$base = `$null
$pick
`$state = Join-Path `$base 'fastcache-cc'
`$log = Join-Path `$state 'invocations.log'
switch (`$args[0]) {
    '--show-stats' { if ((Test-Path -LiteralPath `$log) -and (Get-Item -LiteralPath `$log).Length -gt 0) { "fastcache-cc statistics (`$log)" } else { "fastcache-cc: no statistics recorded yet (`$log)." } }
    '-z' { Remove-Item -LiteralPath `$log -ErrorAction SilentlyContinue }
    default { if (-not `$env:FASTCACHE_NO_STATS) { New-Item -ItemType Directory -Force `$state | Out-Null; Add-Content -LiteralPath `$log ('$logLine'.Replace('@SOURCE@', `$args[-1])) } }
}
"@
    $fixture = Join-Path $work 'fixture.ps1'
    Set-Content -LiteralPath $fixture -Value 'param([string]$Launcher)
Use-E2ELauncherState $h { & $Launcher -z }'
    $savedEnv = @{}
    foreach ($row in $rows) { $savedEnv[$row.Name] = [Environment]::GetEnvironmentVariable($row.Name) }
    $savedSnapshot = $script:LauncherCallerSnapshot
    try {
        foreach ($state in 'present', 'empty', 'absent') {
            # What each case must report, by caller state: nothing, or a finding matching this.
            $expect = @{
                clean = ''; bypass = "of this run's compiles were recorded in the caller's statistics log"
                preEnterZ = $(if ($state -eq 'absent') { '' } else { 'was DELETED during this run' })
            }
            foreach ($case in 'clean', 'bypass', 'preEnterZ') {
                $cases++
                $caller = Join-Path $work "caller-$state-$case"
                $tree = Join-Path $work "tree-$state-$case"
                New-Item -ItemType Directory -Force $tree | Out-Null
                foreach ($row in $rows) { [Environment]::SetEnvironmentVariable($row.Name, $caller) }
                $callerLogs = @($rows | ForEach-Object { Join-Path ($caller + $_.BaseSuffix) $script:LauncherStateLogRelative } | Select-Object -Unique)
                foreach ($log in $callerLogs) {
                    if ($state -eq 'absent') { continue }
                    New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
                    $content = if ($state -eq 'present') { "HIT`tdefault`t1`t2`t/sentinel/a.cpp`n" } else { '' }
                    [IO.File]::WriteAllText($log, $content)
                }
                $before = @($callerLogs | ForEach-Object { if (Test-Path -LiteralPath $_) { (Get-FileHash -LiteralPath $_).Hash } else { '<absent>' } })
                $script:LauncherCallerSnapshot = $null
                $handle = $null
                $damage = @()
                try {
                    Assert-E2ELauncherFixture -Fixture $fixture
                    if ($case -eq 'preEnterZ') { & $fake -z }
                    $handle = Enter-E2ELauncherState -Launcher $fake -Fixture $fixture -RunTrees @($tree)
                    Use-E2ELauncherState $handle { & $fake compile (Join-Path $tree 'a.cpp') }
                    if ((Get-E2ELauncherStateRecordCount $handle) -lt 1) { $failures += "$state/${case}: the positive control counted nothing in the run's own log" }
                    if ($case -eq 'bypass') { & $fake compile (Join-Path $tree 'b.cpp') }
                    Use-E2ELauncherState $handle { & $fake -z }
                } catch {
                    $failures += "$state/${case}: the seam threw: $($_.Exception.Message)"
                } finally {
                    $damage = @(Exit-E2ELauncherState $handle)
                }
                if ($handle -and (Test-Path -LiteralPath $handle.RunRoot)) { $failures += "$state/${case}: Exit left the state root behind ($($handle.RunRoot))" }
                $want = $expect[$case]
                if ($want) {
                    if (-not @($damage | Where-Object { $_ -like "*$want*" }).Count) { $failures += "$state/${case}: expected a finding like '$want', got: $($damage -join '; ')" }
                } else {
                    if ($damage.Count) { $failures += "$state/${case}: expected no finding, got: $($damage -join '; ')" }
                    $after = @($callerLogs | ForEach-Object { if (Test-Path -LiteralPath $_) { (Get-FileHash -LiteralPath $_).Hash } else { '<absent>' } })
                    if (($before -join ',') -ne ($after -join ',')) { $failures += "$state/${case}: the caller's log changed although no finding was reported" }
                }
            }
        }

        # The log read by column NAME: the current version, a line from before versions, and a
        # version this build does not write, which is refused rather than read by position.
        $cases++
        $current = New-E2ELauncherLogLine 'MISS' '/tree/v.cpp'
        $old = "HIT`tdefault`t1`t2`t/tree/old.cpp"
        $read = "$(Get-E2ELauncherLogField $current 'outcome') $(Get-E2ELauncherLogField $current 'source') $(Get-E2ELauncherLogField $old 'outcome') $(Get-E2ELauncherLogField $old 'source')"
        if ($read -ne 'MISS /tree/v.cpp HIT /tree/old.cpp') { $failures += "log field: read [$read], want [MISS /tree/v.cpp HIT /tree/old.cpp]" }
        $cases++
        $refused = $false
        try { $null = Get-E2ELauncherLogField "v999`tMISS`tdefault`t0`t1`t/tree/new.cpp" 'source' } catch { $refused = $true }
        if (-not $refused) { $failures += "log field: a line in a version this build does not write was read by position" }
        # A line in this build's version short of its columns: Stats.cpp refuses it, so must this.
        $cases++
        $short = Read-E2ELauncherLogLine "$logVersion`tMISS`tdefault`t0`t1`t/tree/short.cpp" 'source' (Get-E2ELauncherLogLayout)
        if ($short.Status -ne 'malformed') { $failures += "log field: a $logVersion line short of its columns was read as '$($short.Status)'" }
        # A column name this reader cannot read is a refused LAYOUT, never a column skipped in
        # silence, which would shift every later column by one.
        $cases++
        $odd = Join-Path $work 'Stats.cpp'
        $planted = ([IO.File]::ReadAllText($script:LauncherStateSource)).Replace('.name = "elapsed-ms"', '.name = "elapsed_ms"')
        if (-not $planted.Contains('elapsed_ms')) { $failures += "log layout: the odd column name was not planted" }
        [IO.File]::WriteAllText($odd, $planted)
        $refusedLayout = $false
        try { $null = Get-E2ELauncherLogLayout -Path $odd } catch { $refusedLayout = $true }
        if (-not $refusedLayout) { $failures += "log layout: a column name outside [a-z-] was skipped, not refused" }
        # The caller-damage check's three answers about a record it cannot attribute stay three:
        # a foreign version is that, and a layout it cannot read is that, never "foreign".
        $cases++
        $appendedLog = Join-Path $work 'appended.log'
        [IO.File]::WriteAllText($appendedLog, "v999`tMISS`tdefault`t0`t1`t/tree/new.cpp`n")
        $absentAtStart = [pscustomobject]@{ RunTrees = @($work); RunRoot = $work; CallerSnapshot = @([pscustomobject]@{ Path = $appendedLog; Present = $false }) }
        $foreignDamage = @(Get-E2ELauncherCallerDamage $absentAtStart)
        if (-not @($foreignDamage | Where-Object { $_ -like '*name a log version this build does not write*' }).Count) { $failures += "caller damage: a foreign record read as: $($foreignDamage -join '; ')" }
        $savedSource = $script:LauncherStateSource
        try {
            $script:LauncherStateSource = Join-Path $work 'no-such-Stats.cpp'
            $layoutDamage = @(Get-E2ELauncherCallerDamage $absentAtStart)
        } finally { $script:LauncherStateSource = $savedSource }
        if (-not @($layoutDamage | Where-Object { $_ -like "*log layout could not be read*" }).Count -or @($layoutDamage | Where-Object { $_ -like '*name a log version*' }).Count) {
            $failures += "caller damage: an unreadable layout read as: $($layoutDamage -join '; ')"
        }

        # An unset variable comes back UNSET, and a set one with its value.
        $cases++
        $name = $rows[0].Name
        Remove-Item -LiteralPath "Env:\$name" -ErrorAction SilentlyContinue
        $handle = [pscustomobject]@{ Variables = @($name); Root = $work }
        Use-E2ELauncherState $handle { }
        if (Test-Path -LiteralPath "Env:\$name") { $failures += "restore: $name was unset before the wrapper and exists after it" }
        $cases++
        [Environment]::SetEnvironmentVariable($name, 'caller-value')
        Use-E2ELauncherState $handle { if ([Environment]::GetEnvironmentVariable($name) -ne $work) { throw "not redirected" } }
        if ([Environment]::GetEnvironmentVariable($name) -ne 'caller-value') { $failures += "restore: $name did not come back as the caller's value" }
    }
    finally {
        foreach ($row in $rows) {
            if ($null -eq $savedEnv[$row.Name]) { Remove-Item -LiteralPath "Env:\$($row.Name)" -ErrorAction SilentlyContinue }
            else { [Environment]::SetEnvironmentVariable($row.Name, $savedEnv[$row.Name]) }
        }
        $script:LauncherCallerSnapshot = $savedSnapshot
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
    Write-Host "launcher-state seam self-test: $cases case(s), $($failures.Count) failure(s), on $(Get-E2ELauncherStatePlatform)"
    return $failures
}

Export-ModuleMember -Function Clear-FastcacheEnvironment, Get-E2ELauncherStatePlatform, Get-E2ELauncherStateRows,
    Get-E2EUnwrappedLauncherUse, Test-E2EUnwrappedLauncherScan, Assert-E2ELauncherFixture, Enter-E2ELauncherState,
    Use-E2ELauncherState, Get-E2ELauncherStateRecordCount, Get-E2ELauncherCallerSnapshot, Get-E2ELauncherCallerDamage,
    Exit-E2ELauncherState, Test-E2ELauncherStateSeam, Get-E2ELauncherLogLayout, Get-E2ELauncherLogField,
    Read-E2ELauncherLogLine, New-E2ELauncherLogLine
