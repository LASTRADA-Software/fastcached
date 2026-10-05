# SPDX-License-Identifier: Apache-2.0
#
# End-to-end validation of the fastcache-cc launcher with REAL compilations.
#
# Proves the launcher behaves as a drop-in sccache replacement:
#   1. First compile of a file is a MISS (real compiler runs, result stored).
#   2. Second compile of the same content is a HIT (served from cache, correct
#      object produced) — even after the object is deleted.
#   3. A result past FASTCACHE_MAX_STORE_BYTES is skipped, and the compile still
#      succeeds — a compiler cache must never be able to fail a build. On POSIX
#      the launcher used to stream such an object at a daemon that refuses an
#      over-cap frame and closes, then die of SIGPIPE mid-store with the object
#      already correct on disk (issue #68).
#   4. Cross-depth: content compiled/stored with a DEEP srcroot is a HIT when
#      compiled from a SHALLOW srcroot (different checkout), with showIncludes
#      localized so the deps resolve.
#   5. An EDITED source is a MISS, and yields a different object. The preprocessed
#      text is the only key input carrying the source's content, so a probe that
#      captures none of it answers an edit with the previous revision's object — a
#      wrong build, silently, every time. Checked with direct mode off, because the
#      manifest hashes the source's own bytes and would otherwise mask it. This is
#      the property `/EP` plus `/P` broke: the pair writes the preprocessed text to
#      a FILE, leaving the launcher hashing an empty stdout.
#   6. Moved header: a header that moves with its contents unchanged is a MISS by
#      construction, and the entry stored before the move is still there when it
#      comes back. Preprocessing suppresses line markers, so the token stream and
#      the object are identical after such a move and only the paths differ; the
#      dependency set is part of the key so that the two layouts are two keys
#      rather than one key a replay guard has to catch (issues #53 and #56).
#      Also checked with direct mode off, because the manifest keys on the
#      source's own bytes and would answer the move-back on its own.
#   7. Root-bound objects: a translation unit that bakes its own path into
#      program data -- `__FILE__`, or the builtin `std::source_location` is made
#      of -- is NOT served into a second checkout, in either direct mode, while
#      one that bakes none still is; the first checkout still hits its own copy;
#      and a hit `FASTCACHE_VERIFY` rejects is logged `VERIFY-MISMATCH`, never
#      `HIT`. No key can see those paths -- the direct-mode key never sees the
#      `__FILE__` expansion and `source_location` reaches no key at all -- so
#      the launcher reads the object (apps/fastcache-cc/RootBinding.hpp).
#   9. Roots that do not identify the checkout: a RELATIVE export (`.` and `build`),
#      roots NARROWER than the tree a relative `cl /FC` compile reaches, and roots both
#      checkouts SHARE (one absolute pair exported by both). The second checkout must
#      never be served an object naming the first -- the bound key folds the checkout
#      ABSOLUTE and resolved, and the working directory is scanned and keyed too.
#  10. An 8.3 alias of the build tree: `-I` spelled with every component short while
#      the build tree is exported long. The second build directory must never be
#      served an object naming the first's short path -- the scan reads every alias
#      the key's reconciliation mapped onto a root.
#  11. A junction alias of the build tree, flat, NESTED (a second junction inside the
#      build tree), THROUGH it into the source root, and through a SUBDIRECTORY of it
#      into the source root: the key maps it onto a root, so
#      the scan must know the junction spelling -- every root it names, recorded as the
#      key maps it, walking up the whole spelling.
#   8. Replayed diagnostics: two checkouts sharing a key LEGITIMATELY (nothing in
#      the preprocessed text names a path, direct mode off) -- the second HITs,
#      and every warning and `note:` it replays names the SECOND checkout, with
#      no trace of the first anywhere in either stream. The MSVC family's streams
#      were tagged with a grammar that rewrites /showIncludes notes only, so each
#      diagnostic used to replay the producer's path (value generation 6).
#
# The POSIX counterpart (scripts/compile-cache-e2e.sh) additionally asserts that
# a hit restores the GNU depfile, localized to the consuming checkout. That has
# no analogue here: the MSVC drivers report dependencies inline via
# /showIncludes (asserted above) and set usesDepfile = false, so -MF is never
# parsed and no depfile region is ever stored for them.
#
# Output is generic status only; nothing project-private is read or emitted,
# nothing is committed.
#
# ## The port (#220)
#
# DRAWN per run, from 20000..32000, like every sibling fixture. It used to be the
# constant 21714, and nothing reaped the daemon when a run did not reach its
# cleanup -- so a ctest interrupted, timed out or cancelled left `fastcached.exe`
# listening for as long as the machine was up, and every later run failed at
# `fastcached exited immediately (exit 1)`, naming neither the port nor the
# process.
#
# The reason offered for the constant was real and did not support it: the launcher
# under test reads `FASTCACHE_ADDR` from the environment and several child
# processes inherit it, so the port must be decided BEFORE the daemon starts. That
# is an argument for deciding it early, which drawing does.
#
# Its siblings were checked rather than assumed: `scripts/compile-cache-e2e.sh`
# (the POSIX half of this same test), `scripts/dist-compile-e2e.sh` and
# `scripts/sccache-smoke.sh` all call `free_port`, with `FASTCACHED_SMOKE_PORT` in
# `src/tests/CMakeLists.txt` defaulting to EMPTY so the registration passes no
# `--port`.
#
# An explicitly passed `-Port` is honoured and PROBED first, and a holder is
# refused by name rather than adopted -- a leftover listener is of an unknown
# vintage and may hold a store from a different build, which is the class of
# confusion this fixture exists to detect rather than reproduce. The one exception
# is a listener whose image path is byte-for-byte the daemon this run would start:
# that is reaped, which is not adoption but the manual `Stop-Process` somebody
# already does.
#
# **None of that lives here any more.** It was correct and private, which is how
# two sibling fixtures went on binding a constant with nothing to reach: the draw,
# the probe and the decision are `scripts/lib/E2EPorts.psm1` now, and this file is
# one of its three consumers (#1284).

[CmdletBinding()]
param(
    [string]$Fastcached  = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Launcher    = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcache-cc.exe",
    # 0 means DRAW one, which is what this fixture should always have done and what
    # every sibling already does. A constant made a run that did not reach its
    # cleanup break every later run with `fastcached exited immediately (exit 1)`,
    # naming neither the port nor the process -- diagnosed once by hand from
    # `Get-NetTCPConnection -LocalPort 21714`, after it survived a rebase and read
    # as a regression the rebase had caused (#220). A value passed explicitly is
    # honoured and is then PROBED first, since the caller chose the collision risk.
    [int]$Port           = 0,
    # Left empty on purpose: these default to a directory beside the build tree,
    # computed once the launcher path is resolved. See the note there for why not
    # %TEMP%. Passing one explicitly still works and is honoured verbatim.
    [string]$DeepTemp,
    [string]$ShallowTemp,
    [string]$MoveTemp,
    [string]$EditTemp,
    [string]$AliasTemp,
    [string]$BoundTemp,
    # Drive the port helpers above against real listeners and exit. Needs no
    # daemon, no launcher and no compiler -- which is the point: the decision is
    # what can be wrong, and it was previously reachable only by running a fixture
    # that needs an MSVC toolchain.
    [switch]$SelfTestPorts
)

$ErrorActionPreference = "Stop"
$exit = 0

# Before the skips and the self-test modes, so a machine that skips this fixture still
# judges it: every launcher it runs must sit inside `Use-E2ELauncherState`, or it would
# read or delete the caller's statistics. The why is written in the module, once.
Import-Module (Join-Path $PSScriptRoot "../../../scripts/lib/E2EEnvironment.psm1") -Force
try { Assert-E2ELauncherFixture -Fixture $PSCommandPath }
catch { Write-Host "launcher E2E FAILED: $($_.Exception.Message)"; exit 1 }

# CTest's SKIP_RETURN_CODE. A missing binary or compiler is a missing runtime
# prerequisite, not a failure, so it must be distinguishable from a real fault.
$SKIP = 77
$ranAnyCompiler = $false

# `-SelfTestPorts` needs neither binary -- it drives the port helpers and nothing
# else -- so these skips must not fire ahead of it. Without the guard the self-test
# would report SKIPPED on every machine that has not built the tree, which is a
# check that does not run and does not say so.
if (-not $SelfTestPorts) {
    if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
    if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }
}

# Scratch trees live beside the build tree, not under %TEMP%.
#
# On a GitHub Windows runner %TEMP% is `C:\Users\RUNNER~1\...` — an 8.3 short
# name — and the two drivers disagree about it: `cl` resolves an include through
# the filesystem and reports the LONG name, while clang-cl echoes the spelling it
# was handed. Every root test in the launcher is a string prefix comparison, so a
# short-spelled root matches nothing `cl` emits: PathCanon classifies all of its
# headers as outside both roots, which empties the keyed dependency set AND makes
# the replay guard skip the very paths it exists to check. A moved header then
# keys identically and nothing reports it (measured: "dependency set: 0 of 1
# reported path(s) keyed"). That was a launcher limitation — issue #66, since
# fixed by resolving both the roots and every emitted path through the filesystem
# — and it is still not what these cases are here to measure.
#
# So the roots stay unambiguous here even though the launcher now reconciles them:
# every case above would otherwise be testing the reconciliation as well as its own
# property, and a regression in either would present as a failure of the other. The
# reconciliation has a case of its own instead ("aliased source root"), which
# creates the second spelling deliberately with `subst`.
#
# The build tree is used rather than an expanded short name because there is no
# dependable way to expand one: Resolve-Path, Get-Item and [IO.Path]::GetFullPath
# all preserve it, and Scripting.FileSystemObject was tried and echoed it back
# unchanged. That is also why the aliased-root case substitutes a drive rather than
# asking for an 8.3 name it cannot be sure exists.

# Start-Process resolves a relative -FilePath against the PROCESS working
# directory, not PowerShell's, so a caller passing "out/build/..." would get a
# spurious "file not found". Resolve both up front.
# Guarded for the same reason the skips above are: `-SelfTestPorts` drives the
# port helpers and touches neither binary, and `Resolve-Path` is a hard error for
# a path that does not exist. Everything below this is pure string work, so it is
# left alone.
if (-not $SelfTestPorts) {
    $Fastcached = (Resolve-Path $Fastcached).Path
    $Launcher   = (Resolve-Path $Launcher).Path
}

# AFTER the resolution above, and that ordering is the whole point: both callers
# pass a RELATIVE launcher path, and a relative root is the same defect as a short
# one wearing different clothes. `cl` reports an include as an absolute path no
# matter how it was reached, while clang-cl echoes the spelling it was handed — so
# a relative root matches everything clang-cl emits and nothing `cl` does, which is
# exactly the split that produced "dependency set: 0 of 1" here once before.

$scratch = Join-Path (Split-Path (Split-Path $Launcher -Parent) -Parent) "cc-l-e2e"
if (-not $PSBoundParameters.ContainsKey('DeepTemp'))    { $DeepTemp    = Join-Path $scratch "deep" }
if (-not $PSBoundParameters.ContainsKey('ShallowTemp')) { $ShallowTemp = Join-Path $scratch "shallow" }
if (-not $PSBoundParameters.ContainsKey('MoveTemp'))    { $MoveTemp    = Join-Path $scratch "move" }
if (-not $PSBoundParameters.ContainsKey('EditTemp'))    { $EditTemp    = Join-Path $scratch "edit" }
if (-not $PSBoundParameters.ContainsKey('AliasTemp'))   { $AliasTemp   = Join-Path $scratch "alias" }
if (-not $PSBoundParameters.ContainsKey('BoundTemp'))   { $BoundTemp   = Join-Path $scratch "bound" }

# Belt and braces, and it covers the caller too: a root passed explicitly is
# honoured verbatim, so `-MoveTemp scratch/move` would reintroduce exactly the
# split above. Rooting every one of them here means the cases cannot silently
# degrade into testing clang-cl only — the failure mode this guard exists for is
# a PASS on one driver and a meaningless comparison on the other.
foreach ($name in 'DeepTemp', 'ShallowTemp', 'MoveTemp', 'EditTemp', 'AliasTemp', 'BoundTemp') {
    $value = Get-Variable -Name $name -ValueOnly
    if (-not [System.IO.Path]::IsPathRooted($value)) {
        Set-Variable -Name $name -Value (Join-Path (Get-Location).Path $value)
    }
}

# The per-run port, its holder probe and the refuse-or-reap decision live in
# `scripts/lib/E2EPorts.psm1`.
#
# They were written HERE and were correct here, which is exactly why two sibling
# fixtures went on binding a constant and reaping nothing: a helper is shared only
# if it sits where everything that needs it can include from, and nothing outside
# this file could reach these (#1284). The self-test moved with them, so the
# decision is still driven over staged records and real listeners -- and it now
# also drives every consuming fixture against a held port, which is the half this
# file could not do while it was the only consumer.
#
# Importing it also clears every FASTCACHE_* this process inherited
# (`scripts/lib/E2EEnvironment.psm1`): an operator's FASTCACHE_SCHEDULER would
# dispatch each "local" compile to their fleet. So nothing above sets one.
Import-Module (Join-Path $PSScriptRoot "../../../scripts/lib/E2EPorts.psm1") -Force
# And imported by name as well, for the launcher-state seam: E2EPorts' own import of it
# is nested, so its functions stop at E2EPorts and never reach this script.
Import-Module (Join-Path $PSScriptRoot "../../../scripts/lib/E2EEnvironment.psm1") -Force

if ($SelfTestPorts) { exit (Invoke-E2EPortSelfTest) }

function Start-Fastcached {
    # --storage-max-value raises the wire payload cap along with the value cap;
    # pass it explicitly so the flag stays exercised even on tiny fixtures.
    $p = Start-Process -FilePath $Fastcached `
        -ArgumentList "--bind","127.0.0.1","--port","$Port","--storage-max-value","64M" `
        -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 600
    if ($p.HasExited) { throw "fastcached exited immediately (exit $($p.ExitCode))" }
    return $p
}

# The shared fixture, tagged with the name of the case that built it.
#
# The tag is load-bearing, and it is the same device scripts/compile-cache-e2e.sh
# uses for check_header_move. Paths under SOURCE_DIR are tokenized before hashing,
# which is the whole point of this launcher — so two cases whose trees hold the
# same bytes key IDENTICALLY, and the second one opens on a HIT against the first
# one's entry instead of populating its own. That HIT is correct; it is simply not
# what the second case is about, and it made "launcher cross-depth" pass for a
# reason unrelated to cross-depth sharing: the shallow leg re-created $ShallowTemp
# with content the miss/hit case had already stored from that very directory, down
# to the same /Fo path, so it hit its own earlier entry and would have reported OK
# with cross-depth sharing completely broken.
#
# The tag goes in a string LITERAL rather than a comment: comments do not survive
# preprocessing, and the preprocessed text is what the key is taken over.
function New-Tree([string]$root, [string]$variant) {
    $src = Join-Path $root "src"
    $inc = Join-Path $src "inc"
    New-Item -ItemType Directory -Force -Path $inc | Out-Null
    Set-Content -Path (Join-Path $inc "h1.h") -Value "#pragma once`nint one();`n"
    Set-Content -Path (Join-Path $src "u.cpp") `
                -Value "#include `"inc/h1.h`"`nchar const* variant(){return `"$variant`";}`nint g(){return one();}`n"
    return $src
}

# A single-file fixture whose body can be rewritten between compiles, so an edit
# to the SOURCE (not to a header) is what the cache has to notice.
function Set-EditedSource([string]$src, [int]$value) {
    Set-Content -Path (Join-Path $src "u.cpp") -Value "int value(){return $value;}`n"
}

# Point the fixture's translation unit at a header, wherever it currently lives.
# The include is the ONLY thing that changes across a move: the header's own bytes
# must stay identical, since a move that rewrote them would prove nothing.
function Set-MoveSource([string]$src, [string]$include) {
    Set-Content -Path (Join-Path $src "u.cpp") `
                -Value "#include `"$include`"`nint g(){return answer()-42;}`n"
}

# A tree whose header sits at inc/old, ready to be moved to inc/new.
function New-MoveTree([string]$root) {
    $src = Join-Path $root "src"
    New-Item -ItemType Directory -Force -Path (Join-Path $src "inc/old") | Out-Null
    Set-Content -Path (Join-Path $src "inc/old/h1.h") -Value "#pragma once`ninline int answer(){return 42;}`n"
    Set-MoveSource $src "inc/old/h1.h"
    return $src
}

# Wait for the launcher process ITSELF and return it. Never `Start-Process -Wait`, which in
# PowerShell 7 waits for every descendant too: `cl.exe` on a machine with Visual Studio's
# telemetry on leaves VCTIP.EXE running for minutes after the compile, and the first case sat at
# its first compile for 12 minutes until that one process was stopped (measured, a developer
# machine; CI's images start no VCTIP). The streams are files, so nothing is left unread.
function Wait-LauncherProcess($process) {
    $null = $process.Handle
    $process.WaitForExit()
    return $process
}

# Run the launcher once; return @{ code; stderr } and capture whether it was a
# HIT or MISS from the verbose trace.
function Invoke-Launcher([string]$compiler, [string]$srcRoot, [string]$buildTree, [string]$obj) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$Port"
    $env:FASTCACHE_SOURCE_DIR = $srcRoot
    $env:FASTCACHE_BINARY_DIR = $buildTree
    $env:FASTCACHE_VERBOSE    = "1"
    $source = Join-Path $srcRoot "u.cpp"
    $errFile = New-TemporaryFile
    $p = Wait-LauncherProcess (Use-E2ELauncherState $launcherState {
        Start-Process -FilePath $Launcher `
            -ArgumentList $compiler,"/nologo","/c","/showIncludes","/Fo$obj",$source `
            -NoNewWindow -PassThru -RedirectStandardError $errFile
    })
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stderr = $err }
}

# Like Invoke-Launcher but with one extra environment variable set for the run,
# and cleared again afterwards so it cannot leak into the cases that follow.
function Invoke-LauncherWithEnv([string]$compiler, [string]$srcRoot, [string]$buildTree,
                                [string]$obj, [string]$name, [string]$value) {
    Set-Item -Path "env:$name" -Value $value
    try   { return Invoke-Launcher $compiler $srcRoot $buildTree $obj }
    finally { Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue }
}

# Map a free drive letter onto $target, giving one directory a SECOND spelling.
#
# `subst` is used rather than an 8.3 short name because a short name cannot be
# produced on demand: 8.3 creation is disabled on many volumes, and no PowerShell
# API dependably returns one (Resolve-Path, Get-Item and [IO.Path]::GetFullPath
# all preserve whatever they were given; Scripting.FileSystemObject was tried and
# echoed the input back). A substituted drive reproduces the same condition — one
# directory, two spellings, and `cl` resolving an include through the filesystem
# while clang-cl echoes the spelling it was handed.
#
# Returns the drive letter with its colon (e.g. "X:"), or $null when no letter is
# free or `subst` is unavailable, which is a skip rather than a failure.
function New-SubstDrive([string]$target) {
    # Probed rather than invoked blind: $ErrorActionPreference is "Stop", so a
    # missing subst.exe would be a TERMINATING error and would take the whole
    # script down instead of skipping one case.
    if (-not (Get-Command subst -ErrorAction SilentlyContinue)) { return $null }
    $used = @((Get-PSDrive -PSProvider FileSystem).Name)
    foreach ($letter in 'X','Y','W','V','U','T') {
        if ($used -contains $letter) { continue }
        # Guarded, and the $LASTEXITCODE check below is why: from PowerShell 7.4
        # $PSNativeCommandUseErrorActionPreference defaults to $true, so a native
        # command's non-zero exit is a TERMINATING error under the
        # $ErrorActionPreference = "Stop" set at the top of this file. Unguarded,
        # a drive letter subst declines would abort the whole script — there is no
        # catch on the outer try/finally — instead of skipping one case.
        try { & subst "${letter}:" $target 2>&1 | Out-Null } catch { continue }
        if ($LASTEXITCODE -eq 0) { return "${letter}:" }
    }
    return $null
}

function Remove-SubstDrive([string]$drive) {
    # Same guard, and it matters more here: this runs from a `finally`, where a
    # throw would replace whatever the case was actually reporting.
    if ($drive) { try { & subst /D $drive 2>&1 | Out-Null } catch { } }
}

function Get-Outcome([string]$stderr) {
    if ($stderr -match "fastcache-cc: HIT")  { return "HIT" }
    if ($stderr -match "fastcache-cc: MISS") { return "MISS" }
    return "UNKNOWN"
}

# The outcome with its root-binding qualifier, for the root-bound case: a HIT or a
# MISS that went through a root-bound marker says so on the trace line, and a hit the
# verifier rejected is its own word. Anchored on the launcher's own trace spelling.
function Get-BoundOutcome([string]$stderr) {
    if ($stderr -match "fastcache-cc: VERIFY-MISMATCH key=") { return "VERIFY-MISMATCH" }
    if ($stderr -match "fastcache-cc: HIT key=\S+ \(root-bound:")  { return "BOUND-HIT" }
    if ($stderr -match "fastcache-cc: MISS key=\S+ \(root-bound:") { return "BOUND-MISS" }
    return Get-Outcome $stderr
}

# Whether the file at $path holds $text as bytes, in its narrow spelling. A root-bound
# object served into the wrong checkout holds the OTHER checkout's root; one this
# checkout compiled holds none of it -- which is what the root-bound case asserts.
function Test-ObjectNames([string]$path, [string]$text) {
    if (-not (Test-Path $path)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    return [System.Text.Encoding]::Latin1.GetString($bytes).Contains($text)
}

# A two-checkout tree for the root-bound case: identical bytes in both, tagged per
# case so no case opens on another's entry (see New-Tree for why the tag is a string
# literal).
#
# It includes a PROJECT header, and that is load-bearing: a translation unit that
# reports no dependency records no direct-mode manifest (`DepsNotObserved`), so the
# "direct on" legs would never reach direct mode -- where `__FILE__` was mis-served --
# and would pass with the fix removed. Neutering the store half showed exactly that
# before the header was added; the legs now also assert the manifest was stored.
function New-BoundTree([string]$root, [string]$kind, [string]$tag) {
    $src = Join-Path $root "src"
    New-Item -ItemType Directory -Force -Path (Join-Path $src "inc"), (Join-Path $root "build") | Out-Null
    $body = switch ($kind) {
        'file'   { "char const* Where(){return __FILE__;}" }
        'srcloc' { "char const* Where(){return __builtin_FILE();}" }
        default  { "char const* Where(){return `"nowhere`";}" }
    }
    Set-Content -Path (Join-Path $src "inc/h1.h") -Value "#pragma once`ninline int one(){return 1;}`n"
    Set-Content -Path (Join-Path $src "u.cpp") `
                -Value "#include `"inc/h1.h`"`nchar const* tag(){return `"$tag`";}`n$body`nint g(){return one();}`n"
    return $src
}

# A two-checkout tree whose compile WARNS, for the replayed-diagnostics case: a
# header warning (C4100 / -Wunused-parameter), a TU-level one whose path is the
# source itself (C4101 / -Wunused-variable), and a deprecation, which clang-cl
# follows with a `note:` in the header and an `In file included from` chain. The
# pragmas lift each to the default level, so nothing about the invocation changes.
# Nothing in it names a path, so both checkouts key alike by design.
function New-WarnTree([string]$root, [string]$tag) {
    $src = Join-Path $root "src"
    New-Item -ItemType Directory -Force -Path (Join-Path $src "inc"), (Join-Path $root "build") | Out-Null
    Set-Content -Path (Join-Path $src "inc/probe.h") -Value (
        "#pragma once`n#ifdef __clang__`n#pragma clang diagnostic warning `"-Wunused-parameter`"`n" +
        "#pragma clang diagnostic warning `"-Wunused-variable`"`n#else`n#pragma warning(1: 4100)`n" +
        "#pragma warning(1: 4101)`n#pragma warning(1: 4996)`n#endif`n" +
        "[[deprecated]] inline int old_api() { return 1; }`ninline int unused_param(int x) { return 0; }`n")
    Set-Content -Path (Join-Path $src "u.cpp") `
                -Value "#include `"inc/probe.h`"`nchar const* tag(){return `"$tag`";}`nint use() { int z; return old_api(); }`n"
    return $src
}

# A tree whose header carries the `#pragma message(__FILE__ "(" STR(__LINE__) ") : ...")`
# idiom MSVC's documentation gives for a clickable TODO. `#pragma message` puts nothing
# into the object, so the root binding never sees it and direct mode shares the manifest
# across checkouts: the stream grammar is the only thing keeping the message local.
function New-PragmaTree([string]$root, [string]$tag) {
    $src = Join-Path $root "src"
    New-Item -ItemType Directory -Force -Path (Join-Path $src "inc"), (Join-Path $root "build") | Out-Null
    Set-Content -Path (Join-Path $src "inc/probe.h") -Value (
        "#pragma once`n#define FC_STR2(x) #x`n#define FC_STR1(x) FC_STR2(x)`n" +
        "#pragma message(__FILE__ `"(`" FC_STR1(__LINE__) `") : warning: TODO fix this`")`n" +
        "inline int one() { return 1; }`n")
    Set-Content -Path (Join-Path $src "u.cpp") `
                -Value "#include `"inc/probe.h`"`nchar const* tag(){return `"$tag`";}`nint g() { return one(); }`n"
    return $src
}

# Like Invoke-Launcher, but capturing STDOUT as well: `cl` writes its diagnostics
# there and `clang-cl` to stderr, and the replayed-diagnostics case reads both.
function Invoke-LauncherStreams([string]$compiler, [string]$srcRoot, [string]$buildTree, [string]$obj) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$Port"
    $env:FASTCACHE_SOURCE_DIR = $srcRoot
    $env:FASTCACHE_BINARY_DIR = $buildTree
    $env:FASTCACHE_VERBOSE    = "1"
    $source = Join-Path $srcRoot "u.cpp"
    $outFile = New-TemporaryFile
    $errFile = New-TemporaryFile
    $p = Wait-LauncherProcess (Use-E2ELauncherState $launcherState {
        Start-Process -FilePath $Launcher `
            -ArgumentList $compiler,"/nologo","/c","/showIncludes","/Fo$obj",$source `
            -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    })
    $out = Get-Content -Raw $outFile -ErrorAction SilentlyContinue
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stdout = [string]$out; stderr = [string]$err }
}

# What a build system reads from one compile: both streams, less the launcher's
# own verbose trace, spelled for a case- and separator-blind search.
function Get-CompilerText($run) {
    $lines = ($run.stdout + "`n" + $run.stderr) -split "`r?`n" | Where-Object { $_ -notmatch '^fastcache-cc: ' }
    return (($lines -join "`n").ToLowerInvariant()).Replace('/', '\')
}

# Run the launcher FROM @cwd with the roots exported exactly as given -- relative ones
# included -- and a compile line of relative paths, which is the shape a relative root and
# `cl /FC` combine into. Returns the exit code and stderr.
function Invoke-LauncherIn([string]$cwd, [string]$srcRoot, [string]$buildTree, [string[]]$compileArgs) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$Port"
    $env:FASTCACHE_SOURCE_DIR = $srcRoot
    $env:FASTCACHE_BINARY_DIR = $buildTree
    $env:FASTCACHE_VERBOSE    = "1"
    $outFile = New-TemporaryFile
    $errFile = New-TemporaryFile
    $p = Wait-LauncherProcess (Use-E2ELauncherState $launcherState {
        Start-Process -FilePath $Launcher -ArgumentList $compileArgs -WorkingDirectory $cwd `
            -NoNewWindow -PassThru -RedirectStandardError $errFile -RedirectStandardOutput $outFile
    })
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stderr = [string]$err }
}

# A source whose object clears 1 MiB. Small fixtures never fill the daemon's
# socket send buffer, so they never reach its park-and-resume path — a stall
# there wedged a real build while every small case here kept passing. `long long`
# rather than `int` halves the element count for the same object size, which
# keeps cl's initializer-list parse time reasonable.
function New-BigTree([string]$root) {
    $src = Join-Path $root "src"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    $count = 150000
    $vals = [int64[]]::new($count)
    for ($i = 0; $i -lt $count; $i++) { $vals[$i] = ($i * 7919) % 2147483647 }
    $text = "extern const long long data[$count];`nconst long long data[$count] = {`n" `
          + [string]::Join(",`n", $vals) `
          + "`n};`nint main(){return (int)data[0];}`n"
    Set-Content -Path (Join-Path $src "big.cpp") -Value $text
    return $src
}

# Like Invoke-Launcher but for an arbitrary source and with a bounded wait. The
# failure this guards is a launcher that never returns, so an unbounded -Wait
# would hang the CI job instead of reporting a regression.
function Invoke-LauncherBounded([string]$compiler, [string]$srcRoot, [string]$buildTree,
                                [string]$obj, [string]$sourceName, [int]$timeoutSec) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$Port"
    $env:FASTCACHE_SOURCE_DIR = $srcRoot
    $env:FASTCACHE_BINARY_DIR = $buildTree
    $env:FASTCACHE_VERBOSE    = "1"
    $source = Join-Path $srcRoot $sourceName
    $errFile = New-TemporaryFile
    # Unwaited, so the redirect is only as long as the START: the child took its copy
    # of the environment when it was created, and the wait below is the caller's.
    $p = Use-E2ELauncherState $launcherState {
        Start-Process -FilePath $Launcher `
            -ArgumentList $compiler,"/nologo","/c","/Fo$obj",$source `
            -NoNewWindow -PassThru -RedirectStandardError $errFile
    }
    $exited = $p.WaitForExit($timeoutSec * 1000)
    if (-not $exited) {
        try { $p.Kill() } catch { }
        Remove-Item $errFile -ErrorAction SilentlyContinue
        return @{ code = -1; stderr = ""; timedOut = $true }
    }
    $err = Get-Content -Raw $errFile -ErrorAction SilentlyContinue
    Remove-Item $errFile -ErrorAction SilentlyContinue
    return @{ code = $p.ExitCode; stderr = $err; timedOut = $false }
}

# --- dead peers ---------------------------------------------------------------
# A cache is an accelerator, so a cache that cannot answer must never end the compile. Every
# other case here talks to a live daemon, which is why making a failed fetch end the
# invocation went unnoticed by all of them. These legs point the launcher at a peer that
# cannot answer in each of the three ways a dead cache can be silent, and the object must
# still be built -- inside a bound, so a hang on one of them is a named failure rather than
# the test's own timeout.

# The launcher's two cache deadlines, set for the dead-peer legs alone. Short so a leg costs
# seconds, and the bound is derived from them, so shortening them tightens it.
$DeadPeerConnectMs = 1000
$DeadPeerTotalMs   = 2000

# How many cache exchanges one compile can spend on a dead peer before it compiles: direct
# mode's manifest round trip, then the object fetch. Never the STORE, which a fetch the cache
# did not serve skips. `fastcache-cc --help` says the same under FASTCACHE_TIMEOUT.
$DeadPeerExchanges = 2

# What ONE exchange can cost against each dead shape -- the deadline that ends it -- from which the
# leg's HANG bound is composed. A table, because the shapes spend different deadlines:
#   never-accepting    the connect completes into the backlog, so the exchange runs out TOTAL
#   refused            the dial fails: at once on Linux, after SYN retries on Windows, never past CONNECT
#   accept-then-reset  the RST arrives at once; at most CONNECT
#
# HOW MANY exchanges a leg spent is COUNTED, never inferred from time. Every cache exchange the
# launcher makes goes through one door that says so on its verbose trace (`cache exchange (...)`),
# and each leg asks for exactly $DeadPeerExchanges such lines. The two shapes whose peer can count
# too -- the reset accepts every connection, and never-accepting's listener keeps every one in its
# backlog (measured on Windows: a connection closed or reset before it is accepted is still
# accepted afterwards) -- must agree with the trace, which is what makes the launcher's own count
# trustworthy on the one shape whose peer cannot count: refused.
#
# Time judged the count twice, and both times it measured the HOST rather than the launcher. On the
# wall clock (round 8) the compile and the process starts rode along; on the launcher's own
# `direct-ms` + `cache-ms` (round 9) a refused leg on a starved runner spent 3616 ms against two
# 1000 ms dials, with that compiler's live baseline at 3242 ms and the next one's at 150. Measured
# here: a launcher FROZEN for 1.5 s inside a refused dial reports ~2810 ms for its two exchanges --
# a deadline cannot fire while the process does not run -- and its trace still counts two. 128
# busy processes on 32 cores moved neither number. The count is the same however slow the machine
# is; no slack on a time is.
#
# Time is still judged where it cannot be fooled by a slow host. A shape whose exchange cannot end
# before a deadline has a FLOOR: N x that deadline, less a timer's resolution. A peer that never
# answers ends each exchange only at TOTAL, and load can only lengthen it -- so a launcher whose
# `cache-ms` reads 0, or is taken at the wrong point, fails it. Refused has no floor although
# Windows runs a refused dial out to CONNECT: that is Windows' SYN-retry behaviour, not this
# launcher's. The reset's exchanges end at once. That shape has a CEILING too, (N + 1) x TOTAL:
# an exchange that ignored or doubled its configured deadline -- a broken FASTCACHE_TIMEOUT read, a
# default creeping in -- reports 2 x N x TOTAL and fails it, while a stall of up to one whole TOTAL
# still passes (CI's worst on that leg was +435 ms). And the wall clock bounds the whole leg as the
# HANG detector: N x cost + cost / 2, plus the baseline, plus $DeadPeerWallSlackMs.
#
# BLIND SPOTS, failing OPEN. A retry INSIDE one exchange -- below the door, in the dial -- is one
# trace line: the counted shapes see it as an extra connection, refused does not see it at all.
# And a WAIT anywhere -- a back-off, a wait before the direct attempt or the compile, a store after
# a failed fetch -- is judged only by the hang bound, while an exchange it makes is a trace line
# and counted. And whether CONNECT is honoured end to end is judged by no leg: refused and the
# reset carry no ceiling, because a stall moves a refused dial past any one tight enough to see a
# doubled CONNECT (round 9: +1616 ms); the deadline MECHANICS are ReactorExchange_test's, and only
# the environment-to-budget wiring of CONNECT is left to the hang bound.
$DeadPeerShapeCostMs = [ordered]@{
    'refused'           = $DeadPeerConnectMs
    'never-accepting'   = $DeadPeerTotalMs
    'accept-then-reset' = $DeadPeerConnectMs
}
# What ONE exchange may take AT MOST against each shape, as the launcher reports it, for the
# ceiling of (N + 1) x this; 0 is no ceiling.
$DeadPeerShapeCeilingMs = [ordered]@{
    'refused'           = 0
    'never-accepting'   = $DeadPeerTotalMs
    'accept-then-reset' = 0
}
# What ONE exchange takes AT LEAST against each shape, for the floor; 0 is no floor.
$DeadPeerShapeFloorMs = [ordered]@{
    'refused'           = 0
    'never-accepting'   = $DeadPeerTotalMs
    'accept-then-reset' = 0
}
# The floor's allowance for a timer firing early by its resolution: Windows' default tick is 15.6 ms.
$DeadPeerTimerToleranceMs = 50

# The object's SHA-256 with the COFF header's TimeDateStamp (bytes 4-7) zeroed: every MSVC
# driver stamps the clock there, so two compiles of one source differ there and nowhere else.
function Get-ObjectDigest([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -ge 8) { [Array]::Clear($bytes, 4, 4) }
    return [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes))
}

# What the wall clock may run past the most N exchanges may take before a leg is called a hang:
# the compile, two process starts and whatever a starved runner adds to them.
$DeadPeerWallSlackMs = 10000

# A loopback port nothing listens on: bound, read and released. What connects to it is refused.
function Get-RefusedPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

# A started loopback listener. Never accepted from, the kernel still completes every handshake
# into its backlog, so a client connects, writes its request and waits for a reply nobody sends.
function New-SilentListener {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    return $listener
}

# Run the launcher against $addr under the dead-peer deadlines, stopping it once $boundMs of a
# MONOTONIC clock has passed. With $resetOn, every connection that listener takes while the
# launcher runs is accepted and then reset (a zero linger turns the close into an RST). The
# launcher records into $state, a directory of this leg's own, so its one record is read back.
# Returns @{ code; stderr; elapsedMs; timedOut; resets; spentMs; exchanges }, `spentMs` being the
# launcher's own `direct-ms` + `cache-ms`, or $null when it left not exactly one record, and
# `exchanges` the `cache exchange (...)` lines on its verbose trace.
function Invoke-LauncherDeadPeer([string]$compiler, [string]$srcRoot, [string]$buildTree, [string]$obj,
                                 [string]$addr, [long]$boundMs, $resetOn, [string]$state) {
    $env:FASTCACHE_ADDR            = $addr
    $env:FASTCACHE_SOURCE_DIR      = $srcRoot
    $env:FASTCACHE_BINARY_DIR      = $buildTree
    $env:FASTCACHE_VERBOSE         = "1"
    $env:FASTCACHE_CONNECT_TIMEOUT = "${DeadPeerConnectMs}ms"
    $env:FASTCACHE_TIMEOUT         = "${DeadPeerTotalMs}ms"
    $errFile = New-TemporaryFile
    try {
        $source = Join-Path $srcRoot "u.cpp"
        Remove-Item -Recurse -Force $state -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $state | Out-Null
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        # Unwaited, as in Invoke-LauncherBounded: the child took its environment when it
        # started, and the wait below is the caller's. Under a state root of the leg's own,
        # re-pointed on the handle as the verified-hit case does.
        $launcherState.Root = $state
        try {
            $p = Use-E2ELauncherState $launcherState {
                Start-Process -FilePath $Launcher `
                    -ArgumentList $compiler,"/nologo","/c","/Fo$obj",$source `
                    -NoNewWindow -PassThru -RedirectStandardError $errFile
            }
        } finally {
            $launcherState.Root = $launcherState.RunRoot
        }
        # Read now, or an unwaited Start-Process leaves ExitCode empty once the process is gone.
        $null = $p.Handle
        $resets = 0
        while (-not $p.HasExited -and $clock.ElapsedMilliseconds -lt $boundMs) {
            if ($null -ne $resetOn -and $resetOn.Pending()) {
                $client = $resetOn.AcceptTcpClient()
                $client.Client.LingerState = [System.Net.Sockets.LingerOption]::new($true, 0)
                $client.Close()
                $resets++
            } else {
                $null = $p.WaitForExit(5)
            }
        }
        $elapsed = $clock.ElapsedMilliseconds
        $timedOut = -not $p.HasExited
        if ($timedOut) {
            try { $p.Kill($true) } catch { }
            $null = $p.WaitForExit(10000)
        }
        $err = [string](Get-Content -Raw $errFile -ErrorAction SilentlyContinue)
        $code = if ($timedOut) { -1 } else { $p.ExitCode }
        $records = @(Get-Content (Join-Path $state "fastcache-cc\invocations.log") -ErrorAction SilentlyContinue)
        $spent = if ($records.Count -eq 1) {
            [long](Get-E2ELauncherLogField $records[0] 'direct-ms') + [long](Get-E2ELauncherLogField $records[0] 'cache-ms')
        } else { $null }
        $exchanges = @($err -split "`r?`n" | Where-Object { $_ -match '^fastcache-cc: cache exchange \(' }).Count
        return @{ code = $code; stderr = $err; elapsedMs = $elapsed; timedOut = $timedOut; resets = $resets; spentMs = $spent; exchanges = $exchanges }
    } finally {
        Remove-Item $errFile -ErrorAction SilentlyContinue
        Remove-Item Env:\FASTCACHE_CONNECT_TIMEOUT, Env:\FASTCACHE_TIMEOUT -ErrorAction SilentlyContinue
    }
}

# The dead-peer case for one compiler. The BASELINE is the same compile against the live
# daemon, timed on the same clock: what a compile costs when the cache works. A dead peer may
# add at most what its shape's deadline allows per exchange, so each shape's bound is derived
# from `$DeadPeerShapeCostMs` and nothing is picked by hand -- no scheduler is configured, so
# there is no dispatch budget to add.
# Returns $true when every leg held.
function Test-DeadPeers([string]$compiler) {
    # The legs point FASTCACHE_ADDR at dead peers. Whatever runs after this case reads the address
    # it had before, restored on every way out -- an early return included.
    $savedAddr = $env:FASTCACHE_ADDR
    try {
        # Under $BoundTemp, the scratch root whose cases each own a tagged subdirectory: already a
        # run tree for the launcher-state seam and already removed on every way out.
        $root = Join-Path $BoundTemp "deadpeer-$compiler"
        Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
        $src = New-Tree $root "deadpeer-$compiler"
        $build = Join-Path $root "build"; New-Item -ItemType Directory -Force $build | Out-Null
        $obj = Join-Path $build "u.obj"

        $state = Join-Path $root "state"
        $base = Invoke-LauncherDeadPeer $compiler $src $build $obj "127.0.0.1:$Port" 300000 $null $state
        if ($base.code -ne 0 -or -not (Test-Path $obj)) {
            Write-Host "  DEAD-PEER FAIL ($compiler): the live baseline did not compile (exit $($base.code), timed out $($base.timedOut))" -ForegroundColor Red
            Write-Host $base.stderr
            return $false
        }
        $expected = Get-ObjectDigest $obj
        Write-Host "  baseline $($base.elapsedMs) ms against the live daemon"

        $ok = $true
        foreach ($shape in $DeadPeerShapeCostMs.Keys) {
            $costMs = $DeadPeerShapeCostMs[$shape]
            $exchangeBoundMs = $DeadPeerExchanges * $costMs + [long]($costMs / 2)
            $exchangeFloorMs = if ($DeadPeerShapeFloorMs[$shape] -gt 0) { $DeadPeerExchanges * $DeadPeerShapeFloorMs[$shape] - $DeadPeerTimerToleranceMs } else { 0 }
            $exchangeCeilingMs = if ($DeadPeerShapeCeilingMs[$shape] -gt 0) { ($DeadPeerExchanges + 1) * $DeadPeerShapeCeilingMs[$shape] } else { 0 }
            $boundMs = $base.elapsedMs + $exchangeBoundMs + $DeadPeerWallSlackMs
            Write-Host "  $shape : exactly $DeadPeerExchanges exchange(s); hang bound $boundMs ms = baseline + $DeadPeerExchanges x $costMs + $costMs / 2 + $DeadPeerWallSlackMs ms"
            Remove-Item $obj -Force -ErrorAction SilentlyContinue
            $listener = $null
            try {
                $addr = switch ($shape) {
                    'refused' { "127.0.0.1:$(Get-RefusedPort)" }
                    default   { $listener = New-SilentListener; "127.0.0.1:$($listener.LocalEndpoint.Port)" }
                }
                $resetOn = if ($shape -eq 'accept-then-reset') { $listener } else { $null }
                $r = Invoke-LauncherDeadPeer $compiler $src $build $obj $addr $boundMs $resetOn $state
                # A listener nothing accepted from still holds every connection the launcher
                # made: drained, each one is an exchange, counted as the reset leg counts its own.
                if ($shape -eq 'never-accepting') {
                    while ($listener.Pending()) { $listener.AcceptTcpClient().Close(); $r.resets++ }
                }
            } finally {
                if ($listener) { $listener.Stop() }
            }
            # What tells this leg from one that never reached the peer: the launcher says it fell
            # back. And on the reset leg, where every exchange is a connection this fixture accepts,
            # it COUNTS them: exactly $DeadPeerExchanges, since a time bound cannot see one exchange
            # too many on a shape whose exchanges cost nothing -- a launcher that retried a failed
            # fetch once would pass every bound here and still fail this.
            $fellBack = $r.stderr -match '\(fetch exchange failed\)'
            $counted = $shape -in @('accept-then-reset', 'never-accepting')
            $traced = $r.exchanges -eq $DeadPeerExchanges
            $reached = (-not $counted) -or $r.resets -eq $r.exchanges
            $aboveFloor = ($null -ne $r.spentMs) -and $r.spentMs -ge $exchangeFloorMs
            $belowCeiling = ($exchangeCeilingMs -eq 0) -or (($null -ne $r.spentMs) -and $r.spentMs -le $exchangeCeilingMs)
            $built = (Test-Path $obj) -and ((Get-ObjectDigest $obj) -eq $expected)
            if (-not $r.timedOut -and $r.code -eq 0 -and $built -and $fellBack -and $traced -and $reached -and $aboveFloor -and $belowCeiling) {
                Write-Host "  $shape : compiled locally in $($r.elapsedMs) ms after $($r.exchanges) exchange(s), $($r.spentMs) ms of it on the cache (floor $exchangeFloorMs, ceiling $exchangeCeilingMs), object matches the baseline: OK ($compiler)" -ForegroundColor Green
                continue
            }
            $why = if ($r.timedOut) { "still running at the $boundMs ms hang bound, stopped" }
                   elseif ($r.code -ne 0) { "exit $($r.code)" }
                   elseif (-not $built) { "no object, or not the baseline's" }
                   elseif (-not $fellBack) { "no 'fetch exchange failed' fall-back, so the peer was never asked" }
                   elseif (-not $traced) { "the launcher traced $($r.exchanges) cache exchange(s), want exactly $DeadPeerExchanges" }
                   elseif (-not $reached) { "the peer took $($r.resets) connection(s) while the launcher traced $($r.exchanges) exchange(s) -- its own count is not to be trusted" }
                   elseif ($null -eq $r.spentMs) { "the launcher left no single invocation record, so the time it spent on the cache is unknown" }
                   elseif (-not $aboveFloor) { "the launcher reported $($r.spentMs) ms on the cache, under the $exchangeFloorMs ms that $DeadPeerExchanges exchange(s) with a silent peer cannot end before -- its own timer is not measuring the exchanges" }
                   else { "the launcher reported $($r.spentMs) ms on the cache, past the $exchangeCeilingMs ms ceiling of $DeadPeerExchanges exchange(s) and one more deadline -- an exchange overran its configured deadline" }
            Write-Host "  DEAD-PEER FAIL ($compiler, $shape): $why; $($r.elapsedMs) ms, $($r.exchanges) traced exchange(s), $($r.resets) connection(s)" -ForegroundColor Red
            Write-Host $r.stderr
            $ok = $false
        }
        return $ok
    } finally {
        if ($null -eq $savedAddr) { Remove-Item Env:\FASTCACHE_ADDR -ErrorAction SilentlyContinue }
        else { $env:FASTCACHE_ADDR = $savedAddr }
    }
}

# Settled here, before the daemon starts and before anything reads
# `FASTCACHE_ADDR`, which is the one real constraint on this fixture: the launcher
# under test takes the address from the environment and several child processes
# inherit it. That is a reason to decide the port EARLY, and it was mistaken for a
# reason to make it a constant (#220).
$Port = Get-E2EFixturePort $Port $Fastcached "fastcached"

# Entered here, after every skip, so a skipped run leaves nothing in TEMP -- and inside
# the `try` whose `finally` removes it, before the daemon or any compile, since it is
# what asks the launcher where it will record.
$launcherState = $null
$server = $null
try {
    $launcherState = Enter-E2ELauncherState -Launcher $Launcher -Fixture $PSCommandPath `
        -RunTrees @($DeepTemp, $ShallowTemp, $MoveTemp, $EditTemp, $AliasTemp, $BoundTemp)
    Write-Host "launcher statistics isolated to $($launcherState.Log)"
    $server = Start-Fastcached

    foreach ($cc in @("cl","clang-cl")) {
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) { Write-Host "skip $cc (not on PATH)"; continue }
        $ranAnyCompiler = $true
        Write-Host "=== launcher miss/hit ($cc) ==="
        Remove-Item -Recurse -Force $ShallowTemp -ErrorAction SilentlyContinue
        $src = New-Tree $ShallowTemp "misshit"
        $build = Join-Path $ShallowTemp "build"; New-Item -ItemType Directory -Force $build | Out-Null
        $obj = Join-Path $build "u.obj"

        $r1 = Invoke-Launcher $cc $src $build $obj
        if ($r1.code -ne 0) { Write-Host "  compile 1 failed" -ForegroundColor Red; $exit=1; continue }
        $o1 = Get-Outcome $r1.stderr
        $hash1 = (Get-FileHash $obj -Algorithm SHA256).Hash

        # Delete the object so a HIT must actually reproduce it from cache.
        Remove-Item $obj -Force
        $r2 = Invoke-Launcher $cc $src $build $obj
        $o2 = Get-Outcome $r2.stderr
        $hash2 = if (Test-Path $obj) { (Get-FileHash $obj -Algorithm SHA256).Hash } else { "<none>" }

        if ($o1 -eq "MISS" -and $o2 -eq "HIT" -and $hash2 -eq $hash1) {
            Write-Host "  MISS then HIT, object reproduced identically: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host "  FAIL ($cc): run1=$o1 run2=$o2 objMatch=$($hash2 -eq $hash1)" -ForegroundColor Red
            $exit = 1
        }

        # A result too large to be worth caching must cost the build nothing. On
        # POSIX this is where the launcher used to die: it streamed the object at
        # a daemon that refuses an over-cap frame and closes, and took SIGPIPE
        # mid-store with the object already correct on disk (issue #68). Windows
        # has no SIGPIPE and so cannot reproduce that half, but the ceiling that
        # keeps the transfer from happening at all is the same code on both, and
        # a launcher that mishandled it here would fail a build just as surely.
        Write-Host "=== launcher store ceiling ($cc) ==="
        Remove-Item -Recurse -Force $ShallowTemp -ErrorAction SilentlyContinue
        $ceilSrc = New-Tree $ShallowTemp
        # Re-key off the miss/hit fixture: New-Tree writes byte-identical content
        # every time, so without this the first compile here would be served from
        # the store the first case made and never reach the store path at all.
        Set-EditedSource $ceilSrc 3
        $ceilBuild = Join-Path $ShallowTemp "build"; New-Item -ItemType Directory -Force $ceilBuild | Out-Null
        $ceilObj = Join-Path $ceilBuild "u.obj"

        # 1 byte, so every real object clears it without assuming a size.
        $rCeil = Invoke-LauncherWithEnv $cc $ceilSrc $ceilBuild $ceilObj "FASTCACHE_MAX_STORE_BYTES" "1"
        $oCeil = Get-Outcome $rCeil.stderr
        $ceilExplained = $rCeil.stderr -match "FASTCACHE_MAX_STORE_BYTES"
        $ceilStored = $rCeil.stderr -match "STORED"

        # Nothing was written, so the next compile must MISS again. That is what
        # separates "declined the store" from "stored it and said otherwise".
        Remove-Item $ceilObj -Force -ErrorAction SilentlyContinue
        $oCeil2 = Get-Outcome (Invoke-LauncherWithEnv $cc $ceilSrc $ceilBuild $ceilObj "FASTCACHE_MAX_STORE_BYTES" "1").stderr

        # And the ceiling is opt-out, so a regression leaving it permanently on
        # would surface as a TU that can never be cached.
        Remove-Item $ceilObj -Force -ErrorAction SilentlyContinue
        $rCeilOff = Invoke-LauncherWithEnv $cc $ceilSrc $ceilBuild $ceilObj "FASTCACHE_MAX_STORE_BYTES" "0"

        if ($rCeil.code -eq 0 -and $oCeil -eq "MISS" -and $ceilExplained -and -not $ceilStored `
            -and $oCeil2 -eq "MISS" -and $rCeilOff.stderr -match "STORED") {
            Write-Host "  over-ceiling store declined, compile succeeded, 0 disables: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host ("  CEILING FAIL ($cc): code=$($rCeil.code) run1=$oCeil explained=$ceilExplained " +
                        "stored=$ceilStored run2=$oCeil2 disabled=$($rCeilOff.stderr -match 'STORED')") -ForegroundColor Red
            $exit = 1
        }

        Write-Host "=== launcher large object > 1 MiB ($cc) ==="
        Remove-Item -Recurse -Force $ShallowTemp -ErrorAction SilentlyContinue
        $bigSrc = New-BigTree $ShallowTemp
        $bigBuild = Join-Path $ShallowTemp "build"; New-Item -ItemType Directory -Force $bigBuild | Out-Null
        $bigObj = Join-Path $bigBuild "big.obj"

        $b1 = Invoke-LauncherBounded $cc $bigSrc $bigBuild $bigObj "big.cpp" 300
        if ($b1.timedOut -or $b1.code -ne 0 -or -not (Test-Path $bigObj)) {
            Write-Host "  LARGE FAIL ($cc): first compile did not produce an object (timedOut=$($b1.timedOut))" -ForegroundColor Red
            $exit = 1
        } else {
            $bigBytes = (Get-Item $bigObj).Length
            $bigHash1 = (Get-FileHash $bigObj -Algorithm SHA256).Hash
            Remove-Item $bigObj -Force

            $b2 = Invoke-LauncherBounded $cc $bigSrc $bigBuild $bigObj "big.cpp" 300
            $bigOutcome = Get-Outcome $b2.stderr
            $bigHash2 = if (Test-Path $bigObj) { (Get-FileHash $bigObj -Algorithm SHA256).Hash } else { "<none>" }

            if ($bigBytes -le 1048576) {
                Write-Host "  LARGE FAIL ($cc): object is only $bigBytes bytes; it must exceed 1 MiB to exercise the park path" -ForegroundColor Red
                $exit = 1
            } elseif ($b2.timedOut) {
                Write-Host "  LARGE FAIL ($cc): FETCH never returned (the daemon stalled mid-reply)" -ForegroundColor Red
                $exit = 1
            } elseif ($bigOutcome -eq "HIT" -and $bigHash2 -eq $bigHash1) {
                Write-Host "  large object ($bigBytes bytes) HIT and reproduced identically: OK ($cc)" -ForegroundColor Green
            } else {
                Write-Host "  LARGE FAIL ($cc): run2=$bigOutcome objMatch=$($bigHash2 -eq $bigHash1)" -ForegroundColor Red
                $exit = 1
            }
        }

        Write-Host "=== launcher cross-depth ($cc) ==="
        # Store from a DEEP root, then compile identical content from a SHALLOW
        # root: the second must be a HIT and produce a correct object.
        #
        # Both legs use the "crossdepth" variant, so they share a key with each
        # other and with NOTHING ELSE in this script — see New-Tree. The deep leg
        # is therefore required to be a MISS, which is what makes the shallow HIT
        # mean that the deep entry was found rather than some earlier case's.
        #
        # The two legs differ in exactly what the launcher must not key on: the
        # checkout depth, and with it the absolute path each passes to /Fo. That
        # was folded into every Windows key until the object output was
        # relativized in its fused spelling as well as its separated one.
        Remove-Item -Recurse -Force $DeepTemp -ErrorAction SilentlyContinue
        $deepSrc = New-Tree (Join-Path $DeepTemp "a/b/c/d") "crossdepth"
        $deepBuild = Join-Path (Split-Path $deepSrc -Parent) "build"; New-Item -ItemType Directory -Force $deepBuild | Out-Null
        $deepObj = Join-Path $deepBuild "u.obj"
        $rDeep = Invoke-Launcher $cc $deepSrc $deepBuild $deepObj
        $oDeep = Get-Outcome $rDeep.stderr
        $deepHash = if (Test-Path $deepObj) { (Get-FileHash $deepObj -Algorithm SHA256).Hash } else { "<none>" }

        # New shallow checkout of identical content.
        Remove-Item -Recurse -Force $ShallowTemp -ErrorAction SilentlyContinue
        $src2 = New-Tree $ShallowTemp "crossdepth"
        $build2 = Join-Path $ShallowTemp "build"; New-Item -ItemType Directory -Force $build2 | Out-Null
        $obj2 = Join-Path $build2 "u.obj"
        $rShallow = Invoke-Launcher $cc $src2 $build2 $obj2
        $oShallow = Get-Outcome $rShallow.stderr
        $shallowHash = if (Test-Path $obj2) { (Get-FileHash $obj2 -Algorithm SHA256).Hash } else { "<none>" }

        # The object must be the DEEP one, byte for byte. A hit that merely
        # produced some correct object would also be satisfied by a fresh
        # compile, which is the outcome this case exists to distinguish.
        if ($oDeep -eq "MISS" -and $oShallow -eq "HIT" -and $shallowHash -eq $deepHash -and $deepHash -ne "<none>") {
            Write-Host "  deep store -> shallow HIT, deep object reproduced: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host "  CROSS-DEPTH FAIL ($cc): deep=$oDeep shallow=$oShallow objMatch=$($shallowHash -eq $deepHash)" -ForegroundColor Red
            $exit = 1
        }

        Write-Host "=== edited source ($cc) ==="
        # Direct mode OFF on purpose: its manifest hashes the source file's own
        # bytes, so it catches an edit no matter what the preprocessed text holds —
        # which is precisely how a probe that captured none of it stayed invisible.
        Remove-Item -Recurse -Force $EditTemp -ErrorAction SilentlyContinue
        $editSrc = Join-Path $EditTemp "src"
        New-Item -ItemType Directory -Force -Path $editSrc | Out-Null
        $editBuild = Join-Path $EditTemp "build"; New-Item -ItemType Directory -Force $editBuild | Out-Null
        $editObj = Join-Path $editBuild "u.obj"
        # Reset per compiler: a throw in the `cl` pass would otherwise leave its
        # values in scope and let the `clang-cl` pass assert against them.
        $oEdited = "UNKNOWN"; $firstHash = "<none>"; $secondHash = "<none>"
        $env:FASTCACHE_NO_DIRECT = "1"
        try {
            Set-EditedSource $editSrc 1
            $rFirst = Invoke-Launcher $cc $editSrc $editBuild $editObj
            # Checked, and Test-Path before Get-FileHash: with $ErrorActionPreference
            # = "Stop" a missing object is a terminating error that unwinds past both
            # finallys and kills the run, so a first-compile failure would be reported
            # as an opaque PowerShell exception instead of an EDITED-SOURCE FAIL.
            if ($rFirst.code -eq 0 -and (Test-Path $editObj)) {
                $firstHash = (Get-FileHash $editObj -Algorithm SHA256).Hash

                Set-EditedSource $editSrc 2
                $oEdited = Get-Outcome (Invoke-Launcher $cc $editSrc $editBuild $editObj).stderr
                if (Test-Path $editObj) { $secondHash = (Get-FileHash $editObj -Algorithm SHA256).Hash }
            }
        } finally {
            Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
        }

        if ($oEdited -eq "MISS" -and $firstHash -ne $secondHash) {
            Write-Host "  an edit re-keys and the object follows the source: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host "  EDITED-SOURCE FAIL ($cc): outcome=$oEdited objectChanged=$($firstHash -ne $secondHash)" `
                -ForegroundColor Red
            $exit = 1
        }

        Write-Host "=== moved header ($cc) ==="
        # The header's CONTENTS do not change, so the preprocessed text is
        # byte-identical and the object stays correct; only the paths move. Before
        # the dependency set reached the key the two layouts collided, the cached
        # /showIncludes notes named a file that no longer existed, and Ninja (which
        # reads them as deps = msvc) rebuilt that TU on every build, forever.
        #
        # Direct mode OFF, for the same reason the edited-source case above turns it
        # off: moving the header back restores u.cpp byte-for-byte, so the MANIFEST
        # key is restored too and the final HIT would arrive through direct mode
        # without the object key ever being computed. The assertion would then pass
        # in exactly the state it exists to reject — including the one where the
        # dependency set is silently empty on this driver.
        Remove-Item -Recurse -Force $MoveTemp -ErrorAction SilentlyContinue
        $moveSrc = New-MoveTree $MoveTemp
        $moveBuild = Join-Path $MoveTemp "build"; New-Item -ItemType Directory -Force $moveBuild | Out-Null
        $moveObj = Join-Path $moveBuild "u.obj"
        # Reset per compiler, the captured runs included: without that a failure in
        # the second pass would dump the first pass's trace and misdirect the
        # diagnosis it exists to serve.
        $oBefore = "UNKNOWN"; $oMoved = "UNKNOWN"; $oBack = "UNKNOWN"; $staleHit = $false
        $rBefore = $null; $rMoved = $null; $rBack = $null
        $env:FASTCACHE_NO_DIRECT = "1"
        try {
            $rBefore = Invoke-Launcher $cc $moveSrc $moveBuild $moveObj
            $oBefore = Get-Outcome $rBefore.stderr

            # -Recurse on the directory removals: without it PowerShell's contract for
            # a non-empty directory is a prompt (interactive) or, under
            # $ErrorActionPreference = "Stop", a terminating error — so any stray
            # artefact a scanner or the compiler leaves behind aborts the run instead
            # of reporting MOVED-HEADER FAIL.
            New-Item -ItemType Directory -Force -Path (Join-Path $moveSrc "inc/new") | Out-Null
            Move-Item (Join-Path $moveSrc "inc/old/h1.h") (Join-Path $moveSrc "inc/new/h1.h")
            Remove-Item -Recurse -Force (Join-Path $moveSrc "inc/old") -ErrorAction SilentlyContinue
            Set-MoveSource $moveSrc "inc/new/h1.h"
            Remove-Item -Force $moveObj -ErrorAction SilentlyContinue

            $rMoved = Invoke-Launcher $cc $moveSrc $moveBuild $moveObj
            $oMoved = Get-Outcome $rMoved.stderr
            # A "STALE HIT" here would mean the move still keyed identically and the
            # replay guard had to catch and discard the value — true before issue #56,
            # and the difference between detecting the collision and not having one.
            $staleHit = [bool]($rMoved.stderr -match "STALE HIT")

            # Move it back. The entry stored BEFORE the move must never have been
            # overwritten, which is what separates two keys from one key plus a guard:
            # a guard-only fix re-stores the moved layout under the shared key and
            # destroys the value the original layout needs.
            New-Item -ItemType Directory -Force -Path (Join-Path $moveSrc "inc/old") | Out-Null
            Move-Item (Join-Path $moveSrc "inc/new/h1.h") (Join-Path $moveSrc "inc/old/h1.h")
            Remove-Item -Recurse -Force (Join-Path $moveSrc "inc/new") -ErrorAction SilentlyContinue
            Set-MoveSource $moveSrc "inc/old/h1.h"
            Remove-Item -Force $moveObj -ErrorAction SilentlyContinue

            $rBack = Invoke-Launcher $cc $moveSrc $moveBuild $moveObj
            $oBack = Get-Outcome $rBack.stderr
            # A restored layout that has to discard what it fetched is the collapse
            # this case is named for, and it reports HIT on its way to a MISS.
            if ($rBack.stderr -match "STALE HIT") { $staleHit = $true }
        } finally {
            Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
        }

        if ($oBefore -eq "MISS" -and $oMoved -eq "MISS" -and -not $staleHit -and $oBack -eq "HIT") {
            Write-Host "  moved header keyed apart, pre-move entry survived: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host "  MOVED-HEADER FAIL ($cc): before=$oBefore moved=$oMoved stale=$staleHit back=$oBack" `
                -ForegroundColor Red
            # The launcher's own verbose trace, not just the verdict. This case can
            # only fail in ways that are invisible from outside — an empty dependency
            # set keys the two layouts together, and the "N of M reported path(s)
            # keyed" line separates "the driver reported nothing on the preprocess
            # line" from "every reported path was filtered out". Without this, each
            # diagnosis costs a full CI round trip, and this harness runs on a
            # platform that cannot be reproduced locally.
            # The root as the launcher was given it, next to the paths the driver
            # emitted: a root that does not share a spelling with them (an 8.3 short
            # component, a substituted drive) canonicalizes nothing, which empties
            # the set and silences the replay guard at the same time — and looks
            # exactly like a driver that reported nothing.
            Write-Host "  source root: $moveSrc"
            Write-Host "  tree now: $((Get-ChildItem -Recurse -File $moveSrc | ForEach-Object FullName) -join ', ')"
            foreach ($leg in @(@{n="before"; r=$rBefore}, @{n="moved"; r=$rMoved}, @{n="back"; r=$rBack})) {
                Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                if ($leg.r) { Write-Host $leg.r.stderr }
            }
            $exit = 1
        }

        # --- a root spelled differently from what the driver emits -----------
        # Every root test in the launcher is a string prefix comparison, so a root
        # whose spelling differs from the one the compiler echoes back matches
        # NOTHING it emits. Three mechanisms then fail at once and conceal each
        # other: the keyed dependency set is empty (so a moved header keys
        # identically, and the case above passes for the wrong reason), the replay
        # guard classifies every path as toolchain and probes none of them, and the
        # stored value keeps this machine's absolute paths. The launcher reports
        # ordinary hits throughout — issue #66, measured on a GitHub runner where
        # %TEMP% carries an 8.3 short component.
        #
        # The ROOTS are the substituted spelling and the compile is driven entirely
        # through it, which is the measured shape: `cl` resolves an include through
        # the filesystem and reports the real path, so a subst-spelled root matches
        # nothing it emits, while clang-cl echoes what it was handed and matches
        # everything. Both drivers run, and the case only means something because
        # they disagree.
        #
        # Direct mode is off because this is about the PREPROCESSED key:
        # KeyDependencySet and the "dependency set: N of M reported path(s) keyed"
        # line exist only on that path, and a direct hit reaches the object without
        # ever computing them.
        Write-Host "=== launcher aliased source root ($cc) ==="
        Remove-Item -Recurse -Force $AliasTemp -ErrorAction SilentlyContinue
        $aliasSrc = New-Tree $AliasTemp "aliasedroot"
        $aliasBuild = Join-Path $AliasTemp "build"
        New-Item -ItemType Directory -Force $aliasBuild | Out-Null
        $drive = New-SubstDrive $AliasTemp
        if (-not $drive) {
            Write-Host "  subst unavailable or no free drive letter; skipping ($cc)" -ForegroundColor Yellow
        } else {
            $env:FASTCACHE_NO_DIRECT = "1"
            try {
                $substSrc = Join-Path "$drive\" "src"
                $substBuild = Join-Path "$drive\" "build"
                $aliasObj = Join-Path $aliasBuild "u.obj"

                # Leg 1, through the substituted spelling: this is the leg that
                # fails before the fix, and it fails silently.
                #
                # Both legs name the object by its REAL path, and that is not
                # incidental: a fused `/Fo<path>` is treated as an option and left
                # verbatim in the key (`RelativizeOne` splits only the include-dir
                # prefixes), so spelling it two ways would key the legs apart for a
                # reason that has nothing to do with root reconciliation and would
                # make this case fail whether or not the fix works. Everything the
                # case does measure — the source argument, the include path, and
                # every path `/showIncludes` reports — still differs between them.
                $rAlias = Invoke-Launcher $cc $substSrc $substBuild $aliasObj
                $oAlias = Get-Outcome $rAlias.stderr

                # "0 of M" is the signature, and it is a DIFFERENT fault from
                # "0 of 0" (a driver that reported nothing on the preprocess line).
                $emptySet = $rAlias.stderr -match "dependency set: 0 of [1-9]"
                $keyedSet = $rAlias.stderr -match "dependency set: [1-9]\d* of [1-9]"
                # Matched against the launcher's actual wording. A pattern that
                # matches nothing makes the `-not $warned` half of the pass
                # condition vacuously true, which is a check that cannot fail.
                $warned   = $rAlias.stderr -match "do not contain this translation unit"

                # Leg 2, through the real spelling: same tree, same relative
                # layout, so it must reach leg 1's entry. Before the fix the two
                # legs keyed apart on `cl` and this MISSed.
                Remove-Item $aliasObj -Force -ErrorAction SilentlyContinue
                $rReal = Invoke-Launcher $cc $aliasSrc $aliasBuild $aliasObj
                $oReal = Get-Outcome $rReal.stderr

                if ($rAlias.code -eq 0 -and $oAlias -eq "MISS" -and $keyedSet -and -not $emptySet `
                    -and -not $warned -and $oReal -eq "HIT" -and (Test-Path $aliasObj)) {
                    Write-Host "  two spellings of one source root share a key: OK ($cc)" -ForegroundColor Green
                } else {
                    $why = "  ALIASED-ROOT FAIL ($cc): alias=$oAlias keyed=$keyedSet empty=$emptySet" `
                         + " warned=$warned real=$oReal"
                    Write-Host $why -ForegroundColor Red
                    # The launcher's own trace: this case can only fail in ways
                    # invisible from outside, exactly like the moved-header one.
                    Write-Host "  subst root: $substSrc  real root: $aliasSrc"
                    foreach ($leg in @(@{n="alias"; r=$rAlias}, @{n="real"; r=$rReal})) {
                        Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                        if ($leg.r) { Write-Host $leg.r.stderr }
                    }
                    $exit = 1
                }
            } finally {
                Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
                Remove-SubstDrive $drive
            }
        }
    }

    # --- root-bound objects ---------------------------------------------------
    foreach ($cc in @("cl","clang-cl")) {
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) { continue }
        Write-Host "=== root-bound objects ($cc) ==="
        # Three kinds, two direct modes. `file` and `srcloc` name the compiling
        # checkout in program data; `none` is the control that keeps the fix from
        # passing by binding everything. Before the fix, `file` with direct mode on and
        # `srcloc` in BOTH modes served checkout-a's object into checkout-b.
        #
        # The expected outcome of b is per kind and it is the discrimination: `srcloc`
        # shares a portable key with a (the builtin reaches no key), so b meets a's
        # marker and is a BOUND-MISS; `file` does not share one on the preprocessed path
        # (the expansion is in the hashed text), so after direct mode declines b is an
        # ordinary MISS; `none` is a HIT of a's very bytes.
        $expected = @{ file = "MISS"; srcloc = "BOUND-MISS"; none = "HIT" }
        foreach ($kind in 'file', 'srcloc', 'none') {
            foreach ($direct in 'on', 'off') {
                $tag = "rootbound-$cc-$kind-$direct"
                $case = Join-Path $BoundTemp $tag
                Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
                $rootA = Join-Path $case "checkout-a"
                $rootB = Join-Path $case "checkout-b"
                $srcA = New-BoundTree $rootA $kind $tag
                $srcB = New-BoundTree $rootB $kind $tag
                $objA = Join-Path $rootA "build\u.obj"
                $objB = Join-Path $rootB "build\u.obj"
                if ($direct -eq 'off') { $env:FASTCACHE_NO_DIRECT = "1" }
                try {
                    $rA = Invoke-Launcher $cc $srcA (Join-Path $rootA "build") $objA
                    $rB = Invoke-Launcher $cc $srcB (Join-Path $rootB "build") $objB
                    $hashA = if (Test-Path $objA) { (Get-FileHash $objA -Algorithm SHA256).Hash } else { "<none>" }
                    $hashB = if (Test-Path $objB) { (Get-FileHash $objB -Algorithm SHA256).Hash } else { "<none>" }
                    # And a again, from a deleted object: the checkout that stored a
                    # root-bound copy must still be served it, or the fix bought
                    # correctness by giving up the cache for these translation units.
                    Remove-Item $objA -Force -ErrorAction SilentlyContinue
                    $rA2 = Invoke-Launcher $cc $srcA (Join-Path $rootA "build") $objA
                } finally {
                    Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
                }
                $oA = Get-BoundOutcome $rA.stderr
                $oB = Get-BoundOutcome $rB.stderr
                $oA2 = Get-BoundOutcome $rA2.stderr
                # b's object must not name a's checkout anywhere -- a's object names it
                # in .rdata AND in .debug$S, so a served one always would.
                $bNamesA = Test-ObjectNames $objB $rootA
                $wantA2 = if ($kind -eq 'none') { "HIT" } else { "BOUND-HIT" }
                # With direct mode on, a must have recorded the manifest b then reads, or
                # the leg never exercises direct mode at all.
                $manifest = [bool]($rA.stderr -match "fastcache-cc: MANIFEST stored")
                # And b must have READ it: a manifest a stored and b never validated would turn
                # every direct-on leg back into a direct-off one without a single outcome moving.
                # For a bound object, b's direct mode follows the manifest to a's marker and says so.
                $followed = [bool]($rB.stderr -match "the direct-mode object is root-bound and this checkout has no copy")
                $ok = $rA.code -eq 0 -and $rB.code -eq 0 -and $oA -eq "MISS" -and $oB -eq $expected[$kind] `
                      -and $oA2 -eq $wantA2 -and (Test-Path $objB) -and ($manifest -eq ($direct -eq 'on'))
                if ($kind -eq 'none') { $ok = $ok -and $hashB -eq $hashA }
                else                  { $ok = $ok -and -not $bNamesA -and ($followed -eq ($direct -eq 'on')) }
                if ($ok) {
                    Write-Host "  $kind, direct $direct : a=$oA b=$oB a-again=$oA2 OK ($cc)" -ForegroundColor Green
                } else {
                    Write-Host ("  ROOT-BOUND FAIL ($cc, $kind, direct $direct): a=$oA b=$oB (want $($expected[$kind])) " +
                                "a-again=$oA2 (want $wantA2) b-names-a=$bNamesA sameBytes=$($hashB -eq $hashA) " +
                                "manifest=$manifest b-read-manifest=$followed") -ForegroundColor Red
                    foreach ($leg in @(@{n="a"; r=$rA}, @{n="b"; r=$rB}, @{n="a again"; r=$rA2})) {
                        Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                        if ($leg.r) { Write-Host $leg.r.stderr }
                    }
                    $exit = 1
                }
            }
        }

        # A verified hit that the verifier rejects is VERIFY-MISMATCH in the log and on
        # the trace, never HIT. `cl` records the object's absolute path in `.debug$S`
        # with no debug flag at all and the verifier does not excuse it, so a
        # cross-checkout `cl` hit is rejected by design; `clang-cl` records no such path
        # and verifies clean. Both directions are asserted, which is what keeps this from
        # passing under a launcher that logged every verified hit one way.
        Write-Host "=== verified cross-checkout hit ($cc) ==="
        $tag = "rootbound-$cc-verify"
        $case = Join-Path $BoundTemp $tag
        Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
        $rootA = Join-Path $case "checkout-a"; $rootB = Join-Path $case "checkout-b"
        $srcA = New-BoundTree $rootA 'none' $tag
        $srcB = New-BoundTree $rootB 'none' $tag
        # A state root of the CASE's own, to count exactly this case's two records, which
        # the run's shared log cannot. It re-points the launcher-state handle rather than
        # LOCALAPPDATA -- the wrapper would overwrite that on every launcher start -- and
        # so moves every state variable, not only the Windows one.
        $state = Join-Path $case "state"
        New-Item -ItemType Directory -Force $state | Out-Null
        $env:FASTCACHE_NO_DIRECT = "1"
        try {
            $launcherState.Root = $state
            $rA = Invoke-Launcher $cc $srcA (Join-Path $rootA "build") (Join-Path $rootA "build\u.obj")
            $rB = Invoke-LauncherWithEnv $cc $srcB (Join-Path $rootB "build") (Join-Path $rootB "build\u.obj") "FASTCACHE_VERIFY" "1"
        } finally {
            $launcherState.Root = $launcherState.RunRoot
            Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
        }
        $logged = @(Get-Content (Join-Path $state "fastcache-cc\invocations.log") -ErrorAction SilentlyContinue | ForEach-Object { Get-E2ELauncherLogField $_ 'outcome' })
        $oB = Get-BoundOutcome $rB.stderr
        $wrongLine = [bool]($rB.stderr -match "WRONG OBJECT served")
        $want = if ($cc -eq "cl") { "VERIFY-MISMATCH" } else { "HIT" }
        if ($rB.code -eq 0 -and (Get-Outcome $rA.stderr) -eq "MISS" -and $oB -eq $want -and $logged.Count -eq 2 `
            -and $logged[1] -eq $want -and $wrongLine -eq ($want -eq "VERIFY-MISMATCH")) {
            Write-Host "  a verified hit is logged $want, WRONG OBJECT line=${wrongLine}: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host ("  VERIFY FAIL ($cc): b=$oB (want $want) logged=[$($logged -join ',')] wrongLine=$wrongLine") -ForegroundColor Red
            Write-Host $rB.stderr
            $exit = 1
        }

        # A replayed diagnostic names the checkout that REPLAYED it. Direct mode is
        # off so this cannot be the `__FILE__` defect above: nothing here names a
        # path, the two checkouts share a key by design, and the object is correct
        # for both -- what was wrong was the text around it. Three assertions, each
        # for a different way of passing without testing anything: b must HIT (a
        # miss replays its own compile), a must have WARNED (no warning, nothing to
        # rewrite), and b's streams must name b and nowhere name a.
        Write-Host "=== replayed diagnostics ($cc) ==="
        $tag = "replayed-diagnostics-$cc"
        $case = Join-Path $BoundTemp $tag
        Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
        $rootA = Join-Path $case "checkout-a"; $rootB = Join-Path $case "checkout-b"
        $srcA = New-WarnTree $rootA $tag
        $srcB = New-WarnTree $rootB $tag
        $env:FASTCACHE_NO_DIRECT = "1"
        try {
            $rA = Invoke-LauncherStreams $cc $srcA (Join-Path $rootA "build") (Join-Path $rootA "build\u.obj")
            $rB = Invoke-LauncherStreams $cc $srcB (Join-Path $rootB "build") (Join-Path $rootB "build\u.obj")
        } finally {
            Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
        }
        $textA = Get-CompilerText $rA
        $textB = Get-CompilerText $rB
        $spell = { param([string]$path) $path.ToLowerInvariant().Replace('/', '\') }
        $headerA = & $spell (Join-Path $srcA "inc\probe.h(")
        $headerB = & $spell (Join-Path $srcB "inc\probe.h(")
        $sourceB = & $spell (Join-Path $srcB "u.cpp(")
        $warnedA = $textA.Contains($headerA)
        $namesB = $textB.Contains($headerB) -and $textB.Contains($sourceB)
        $namesA = $textB.Contains((& $spell $rootA))
        $chainB = if ($cc -eq "clang-cl") { $textB.Contains("in file included from " + (& $spell (Join-Path $srcB "u.cpp:"))) } else { $true }
        $oA = Get-Outcome $rA.stderr
        $oB = Get-Outcome $rB.stderr
        if ($rA.code -eq 0 -and $rB.code -eq 0 -and $oA -eq "MISS" -and $oB -eq "HIT" -and $warnedA -and $namesB `
            -and -not $namesA -and $chainB) {
            Write-Host "  b HIT, and its replayed diagnostics name b and never a: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host ("  REPLAYED-DIAGNOSTICS FAIL ($cc): a=$oA b=$oB a-warned=$warnedA b-names-b=$namesB " +
                        "b-names-a=$namesA include-chain=$chainB") -ForegroundColor Red
            Write-Host "  --- a ---" -ForegroundColor Yellow; Write-Host $rA.stdout; Write-Host $rA.stderr
            Write-Host "  --- b ---" -ForegroundColor Yellow; Write-Host $rB.stdout; Write-Host $rB.stderr
            $exit = 1
        }

        Write-Host "=== pragma-message head, direct mode on ($cc) ==="
        # The Job 2 review's end-to-end case: the spaced `(line) : ` head, direct mode ON (the
        # default). Measured before the fix: b HIT through a's manifest and its replayed
        # message named checkout a. `cl` writes the idiom's text as the whole line; clang-cl
        # wraps it in its own head, `probe.h(4,9): warning: <the idiom's text>`, which puts the
        # idiom's path MID-line -- a residual shared with every grammar here -- so on clang-cl
        # only the driver's own head is asserted.
        $tag = "pragma-message-$cc"
        $case = Join-Path $BoundTemp $tag
        Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
        $rootA = Join-Path $case "checkout-a"; $rootB = Join-Path $case "checkout-b"
        $srcA = New-PragmaTree $rootA $tag
        $srcB = New-PragmaTree $rootB $tag
        $rA = Invoke-LauncherStreams $cc $srcA (Join-Path $rootA "build") (Join-Path $rootA "build\u.obj")
        $rB = Invoke-LauncherStreams $cc $srcB (Join-Path $rootB "build") (Join-Path $rootB "build\u.obj")
        $textA = Get-CompilerText $rA
        $textB = Get-CompilerText $rB
        $spell = { param([string]$path) $path.ToLowerInvariant().Replace('/', '\') }
        $headB = & $spell (Join-Path $srcB "inc\probe.h(4")
        $printedA = $textA.Contains((& $spell (Join-Path $srcA "inc\probe.h(4"))) -and $textA.Contains("todo fix this")
        $namesB = $textB.Contains($headB) -and $textB.Contains("todo fix this")
        $namesA = $textB.Contains((& $spell $rootA))
        $oA = Get-Outcome $rA.stderr
        $oB = Get-Outcome $rB.stderr
        $direct = [bool]($rB.stderr -match "direct mode: the manifest validated")
        $ok = $rA.code -eq 0 -and $rB.code -eq 0 -and $oA -eq "MISS" -and $oB -eq "HIT" -and $direct -and $printedA -and $namesB
        if ($cc -eq "cl") { $ok = $ok -and -not $namesA }
        if ($ok) {
            Write-Host "  b HIT through direct mode, and its pragma message names b: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host ("  PRAGMA-MESSAGE FAIL ($cc): a=$oA b=$oB b-direct=$direct a-printed=$printedA b-names-b=$namesB " +
                        "b-names-a=$namesA") -ForegroundColor Red
            Write-Host "  --- a ---" -ForegroundColor Yellow; Write-Host $rA.stdout; Write-Host $rA.stderr
            Write-Host "  --- b ---" -ForegroundColor Yellow; Write-Host $rB.stdout; Write-Host $rB.stderr
            $exit = 1
        }

        Write-Host "=== a stale manifest is refused ($cc) ==="
        # Direct mode's whole safety case, in one checkout: an unchanged recompile is served
        # THROUGH the manifest, and one after a header EDIT is not -- the manifest re-hashes
        # every dependency, and a stale one must miss rather than serve the old object. The
        # first half is the control that makes the second mean something (a direct mode that
        # never validates would miss both), and the second is what a direct mode that
        # validates NOTHING gets wrong while every outcome elsewhere in this fixture holds.
        $tag = "stale-manifest-$cc"
        $case = Join-Path $BoundTemp $tag
        Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
        $root = Join-Path $case "checkout"
        $src = New-WarnTree $root $tag
        $obj = Join-Path $root "build\u.obj"
        $rFirst = Invoke-LauncherStreams $cc $src (Join-Path $root "build") $obj
        $firstHash = if (Test-Path $obj) { (Get-FileHash $obj -Algorithm SHA256).Hash } else { "<none>" }
        $rSame = Invoke-LauncherStreams $cc $src (Join-Path $root "build") $obj
        # The edit changes what the TU compiles to, not only its bytes: `old_api()`, which the
        # TU calls, returns 7 instead of 1, so a stale object is a WRONG one here. (An unused
        # inline function would not do: clang-cl's object was byte-identical across it,
        # measured, and `cl` stamps the clock into every object, so only clang-cl's hash is
        # evidence about content at all.)
        $probe = Join-Path $src "inc\probe.h"
        $before = Get-Content -Raw $probe
        Set-Content -Path $probe -Value $before.Replace("old_api() { return 1; }", "old_api() { return 7; }") -NoNewline
        $edited = (Get-Content -Raw $probe) -ne $before
        $rEdited = Invoke-LauncherStreams $cc $src (Join-Path $root "build") $obj
        $editedHash = if (Test-Path $obj) { (Get-FileHash $obj -Algorithm SHA256).Hash } else { "<none>" }
        $manifest = [bool]($rFirst.stderr -match "fastcache-cc: MANIFEST stored")
        $sameDirect = (Get-Outcome $rSame.stderr) -eq "HIT" -and [bool]($rSame.stderr -match "direct mode: the manifest validated")
        $oEdited = Get-Outcome $rEdited.stderr
        $editedDirect = [bool]($rEdited.stderr -match "direct mode: the manifest validated")
        $ok = $rFirst.code -eq 0 -and $rSame.code -eq 0 -and $rEdited.code -eq 0 -and $edited -and $manifest -and $sameDirect `
              -and $oEdited -eq "MISS" -and -not $editedDirect -and $editedHash -ne $firstHash -and $editedHash -ne "<none>"
        if ($ok) {
            Write-Host "  unchanged: HIT through the manifest; header edited: MISS, and the object follows: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host ("  STALE-MANIFEST FAIL ($cc): header-edited=$edited manifest=$manifest unchanged-direct-hit=$sameDirect edited=$oEdited " +
                        "edited-direct=$editedDirect objectChanged=$($editedHash -ne $firstHash)") -ForegroundColor Red
            foreach ($leg in @(@{n="first"; r=$rFirst}, @{n="unchanged"; r=$rSame}, @{n="edited"; r=$rEdited})) {
                Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                Write-Host $leg.r.stderr
            }
            $exit = 1
        }

        # Roots that do not identify the checkout. `relroots` exports `.` and `build`, the
        # same strings in every checkout; `narrow` exports roots that do not cover the tree
        # the compile reaches. Both compile relatively from the checkout with `/FC`, which
        # makes `cl` write the ABSOLUTE path into `__builtin_FILE` -- measured by the review
        # as the second checkout printing the first one's path, `(root-bound: served from
        # key=...)` under `relroots`, a plain HIT under `narrow`. clang-cl keeps the path
        # relative, so its object names no checkout and may be shared; either way b's object
        # must not name a's checkout.
        #
        # `shared` is the Job 1 re-review's case: both checkouts export the SAME absolute
        # roots (`<case>\shared`, `<case>\shared\build`), so the roots say nothing about
        # which checkout compiled, and only the WORKING DIRECTORY -- which `/FC` absolutizes
        # against -- tells them apart. Run for `__FILE__` as well as `source_location`,
        # since the two reach the object by different roads.
        Write-Host "=== roots that do not identify the checkout ($cc) ==="
        foreach ($shape in 'relroots', 'narrow', 'shared') {
            $kinds = if ($shape -eq 'shared') { @('file', 'srcloc') } else { @('srcloc') }
            foreach ($kind in $kinds) {
                foreach ($direct in 'on', 'off') {
                    $tag = "unidentified-$shape-$kind-$cc-$direct"
                    $case = Join-Path $BoundTemp $tag
                    Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
                    $rootA = Join-Path $case "checkout-a"; $rootB = Join-Path $case "checkout-b"
                    $null = New-BoundTree $rootA $kind $tag
                    $null = New-BoundTree $rootB $kind $tag
                    New-Item -ItemType Directory -Force -Path (Join-Path $case "shared\build") | Out-Null
                    $objA = Join-Path $rootA "build\u.obj"; $objB = Join-Path $rootB "build\u.obj"
                    $compileArgs = @($cc, "/nologo", "/FC", "/c", "/showIncludes", "-Isrc", "/Fobuild\u.obj", "src\u.cpp")
                    $roots = {
                        param([string]$root)
                        if ($shape -eq 'relroots') { return @('.', 'build') }
                        if ($shape -eq 'shared') { return @((Join-Path $case "shared"), (Join-Path $case "shared\build")) }
                        return @((Join-Path $root "lib"), (Join-Path $root "build"))
                    }
                    if ($direct -eq 'off') { $env:FASTCACHE_NO_DIRECT = "1" }
                    try {
                        # Named apart from `$rA`/`$rB`: PowerShell variable names are
                        # case-INSENSITIVE, so `$ra` would be `$rA`, overwritten by the first run.
                        $exportA = & $roots $rootA; $exportB = & $roots $rootB
                        $rA = Invoke-LauncherIn $rootA $exportA[0] $exportA[1] $compileArgs
                        $rB = Invoke-LauncherIn $rootB $exportB[0] $exportB[1] $compileArgs
                        Remove-Item $objA -Force -ErrorAction SilentlyContinue
                        $rA2 = Invoke-LauncherIn $rootA $exportA[0] $exportA[1] $compileArgs
                    } finally {
                        Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
                    }
                    $oB = Get-BoundOutcome $rB.stderr
                    $oA2 = Get-BoundOutcome $rA2.stderr
                    $aNamesA = Test-ObjectNames $objA $rootA
                    $bNamesA = Test-ObjectNames $objB $rootA
                    $bNamesB = Test-ObjectNames $objB $rootB
                    # The control that keeps the leg honest: under `cl` the object DOES name the
                    # checkout, so the leg exercises the defect rather than a portable object.
                    $exercised = ($cc -ne 'cl') -or $aNamesA
                    $ok = $rA.code -eq 0 -and $rB.code -eq 0 -and $rA2.code -eq 0 -and $exercised -and -not $bNamesA
                    # And b compiled its OWN copy, which names b: the positive beside the absence.
                    if ($aNamesA) { $ok = $ok -and $oB -ne 'HIT' -and $oA2 -eq 'BOUND-HIT' -and $bNamesB }
                    if ($ok) {
                        Write-Host ("  $shape/$kind, direct $direct : b=$oB a-again=$oA2 a-names-a=$aNamesA " +
                                    "b-names-a=$bNamesA b-names-b=$bNamesB OK ($cc)") -ForegroundColor Green
                    } else {
                        Write-Host ("  UNIDENTIFIED-ROOTS FAIL ($cc, $shape/$kind, direct $direct): b=$oB a-again=$oA2 " +
                                    "a-names-a=$aNamesA b-names-a=$bNamesA b-names-b=$bNamesB " +
                                    "codes=$($rA.code)/$($rB.code)/$($rA2.code)") -ForegroundColor Red
                        foreach ($leg in @(@{n="a"; r=$rA}, @{n="b"; r=$rB}, @{n="a again"; r=$rA2})) {
                            Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                            Write-Host $leg.r.stderr
                        }
                        $exit = 1
                    }
                }
            }
        }
    }

    # --- an 8.3 alias of the build tree -------------------------------------
    # The part-keying review's I1, measured before the fix as a WRONG HIT: two build
    # directories of one checkout, the build tree exported LONG, and `-I` spelled with
    # every component SHORT (`...\BUILDV~1\gen`). The key resolved the short `-I`
    # through the filesystem and tokenized it as the build tree, so both build
    # directories shared a key -- while the root scan knew only the long spellings,
    # judged the object portable, and build directory 2 was served build directory 1's
    # `__builtin_FILE()`. On C:'s temp directory rather than beside the other trees:
    # 8.3 names are a VOLUME setting, and this project's own drive may not make them.
    # A volume that makes none is a loud SKIP, never a pass.
    foreach ($cc in @("cl","clang-cl")) {
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) { continue }
        Write-Host "=== an 8.3 alias of the build tree ($cc) ==="
        foreach ($direct in 'on', 'off') {
            $case = Join-Path ([System.IO.Path]::GetTempPath()) ("fc-cc-83-" + [guid]::NewGuid().ToString('N').Substring(0, 12))
            try {
                $checkout = Join-Path $case "checkoutwithalongname"
                $tag = "alias83-$cc-$direct"
                New-Item -ItemType Directory -Force -Path (Join-Path $checkout "src") | Out-Null
                Set-Content -Path (Join-Path $checkout "src\u.cpp") `
                            -Value "#include `"g.h`"`nchar const* tag(){return `"$tag`";}`nchar const* Where(){return G();}`n"
                $fso = New-Object -ComObject Scripting.FileSystemObject
                $dirs = foreach ($n in 1, 2) {
                    $long = Join-Path $checkout "buildverylongname$n"
                    New-Item -ItemType Directory -Force -Path (Join-Path $long "gen") | Out-Null
                    Set-Content -Path (Join-Path $long "gen\g.h") -Value "#pragma once`ninline char const* G(){return __builtin_FILE();}`n"
                    @{ long = $long; short = $fso.GetFolder($long).ShortPath }
                }
                if ($dirs[0].short -eq $dirs[0].long -or $dirs[1].short -eq $dirs[1].long) {
                    Write-Host "  SKIPPED ($cc, direct $direct): this volume makes no 8.3 names ($($dirs[0].short))" -ForegroundColor Yellow
                    continue
                }
                $compile = {
                    param($dir)
                    $compileArgs = @($cc, "/nologo", "/c", "/showIncludes", "-I$($dir.short)\gen",
                                     "/Fo$($dir.long)\u.obj", (Join-Path $checkout "src\u.cpp"))
                    Invoke-LauncherIn $dir.long $checkout $dir.long $compileArgs
                }
                # Case-insensitive, because which case a driver keeps for a short name is
                # its own business; the question is only WHICH directory the object names.
                $names = {
                    param([string]$obj, [string]$dir)
                    if (-not (Test-Path $obj)) { return $false }
                    $text = [System.Text.Encoding]::Latin1.GetString([System.IO.File]::ReadAllBytes($obj)).ToLowerInvariant()
                    return $text.Contains($dir.ToLowerInvariant())
                }
                if ($direct -eq 'off') { $env:FASTCACHE_NO_DIRECT = "1" }
                try {
                    $r1 = & $compile $dirs[0]
                    $r2 = & $compile $dirs[1]
                    Remove-Item (Join-Path $dirs[0].long "u.obj") -Force -ErrorAction SilentlyContinue
                    $r1again = & $compile $dirs[0]
                } finally {
                    Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
                }
                $obj1 = Join-Path $dirs[0].long "u.obj"; $obj2 = Join-Path $dirs[1].long "u.obj"
                $oneNamesOne = & $names $obj1 $dirs[0].short
                $twoNamesOne = & $names $obj2 $dirs[0].short
                $twoNamesTwo = & $names $obj2 $dirs[1].short
                $o2 = Get-BoundOutcome $r2.stderr
                $o1again = Get-BoundOutcome $r1again.stderr
                # The control first: the object must name the SHORT spelling, or this leg
                # exercises nothing the long needles did not already catch.
                $ok = $r1.code -eq 0 -and $r2.code -eq 0 -and $r1again.code -eq 0 -and $oneNamesOne `
                      -and -not $twoNamesOne -and $twoNamesTwo -and $o2 -ne 'HIT' -and $o1again -eq 'BOUND-HIT'
                if ($ok) {
                    Write-Host "  direct $direct : b2=$o2 b1-again=$o1again, b2 names its own short path: OK ($cc)" -ForegroundColor Green
                } else {
                    Write-Host ("  ALIAS-83 FAIL ($cc, direct $direct): b2=$o2 b1-again=$o1again b1-names-b1=$oneNamesOne " +
                                "b2-names-b1=$twoNamesOne b2-names-b2=$twoNamesTwo codes=$($r1.code)/$($r2.code)/$($r1again.code)") -ForegroundColor Red
                    foreach ($leg in @(@{n="b1"; r=$r1}, @{n="b2"; r=$r2}, @{n="b1 again"; r=$r1again})) {
                        Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                        Write-Host $leg.r.stderr
                    }
                    $exit = 1
                }
            } finally {
                Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
            }
        }
    }

    # --- a junction alias of the build tree -----------------------------------
    # The combined re-review's I1 and M4. `jx\L<n>` is a junction to build directory n,
    # outside both roots, and `-I` reaches the generated header through it:
    #   flat:    -I<run>\jx\L<n>\x\y\gen
    #   nested:  -I<run>\jx\L<n>\genlink, `genlink` a SECOND junction inside the build tree
    #   through: -I<run>\jx\L<n>\srclink\src\gen, `srclink` a junction inside the build tree
    #            back to the SOURCE root, so the alias runs through build directory n into
    #            the source root. Measured before its fix: the object was bound to the
    #            source root alone, and build directory 2 printed a path spelled through
    #            build directory 1's link (`...\jx\L1\srclink\src\gen\g.h`).
    #   subthrough: the same, but `jx\L<n>` is a junction to a SUBDIRECTORY of the build
    #            tree (`b<n>\x`), so no ancestor of the spelling resolves TO build directory
    #            n, only INTO it. Measured before its fix: bound to the source root alone
    #            again, build directory 2 served a path spelled through build directory 1.
    #   selfbuild, selfthrough, srcself, revthrough: the reviewer's twists (`jx2.ps1`) --
    #            the build tree reached twice, twice and then through, the source root
    #            reached twice, and a shared alias of the SOURCE root through a per-build
    #            link to the build tree.
    #   dotdot, dotseg, dotdotsub: flat and subthrough spelled through `jx\zz\..` or `jx\.`,
    #            which both compilers write verbatim with no debug flag. Measured before the
    #            fix, clang-cl's build directory 2 served build directory 1's spelling.
    #   dotdotfc: dotdot under `cl /FC`, which COLLAPSES the spelling in the object (a debug
    #            flag does too): the leg for the normal-form needle, which no verbatim leg
    #            covers. cl only.
    # The key resolves either spelling to `<BUILDTREE>/x/y/gen`, so both build directories
    # share a key, and only the alias list tells the scan that `jx\L1` IS build directory
    # 1. Measured before the fix, nested: build directory 2 HIT and printed
    # `...\jx\L1\genlink\g.h`. The flat leg is the one a recorder that records nothing
    # turns red -- the 8.3 leg above passes on the seeded short forms alone. Junctions,
    # because `mklink /J` needs no elevation; removed as links before the tree is.
    foreach ($cc in @("cl","clang-cl")) {
        if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) { continue }
        Write-Host "=== a junction alias of the build tree ($cc) ==="
        foreach ($shape in 'flat', 'nested', 'through', 'subthrough', 'selfbuild', 'selfthrough', 'srcself', 'revthrough',
                           'dotdot', 'dotseg', 'dotdotsub', 'dotdotfc') {
            # `/FC` is cl's; clang-cl writes the spelling verbatim whatever it is given.
            if ($shape -eq 'dotdotfc' -and $cc -ne 'cl') { continue }
            # Always an array: an `if` yielding nothing is $null, which would reach cl as "".
            $extra = @(if ($shape -eq 'dotdotfc') { '/FC' })
            foreach ($direct in 'on', 'off') {
                $case = Join-Path ([System.IO.Path]::GetTempPath()) ("fc-cc-jx-" + [guid]::NewGuid().ToString('N').Substring(0, 12))
                $links = [System.Collections.Generic.List[string]]::new()
                try {
                    $checkout = Join-Path $case "ck"
                    $tag = "junction-$shape-$cc-$direct"
                    New-Item -ItemType Directory -Force -Path (Join-Path $checkout "src"), (Join-Path $case "jx") | Out-Null
                    Set-Content -Path (Join-Path $checkout "src\u.cpp") `
                                -Value "#include `"g.h`"`nchar const* tag(){return `"$tag`";}`nchar const* Where(){return G();}`n"
                    # One junction, created once and removed as a link before the tree is.
                    $junction = {
                        param([string]$path, [string]$target)
                        if (-not (Test-Path $path)) {
                            New-Item -ItemType Junction -Path $path -Target $target | Out-Null
                            $links.Add($path)
                        }
                    }
                    $header = "#pragma once`ninline char const* G(){return __builtin_FILE();}`n"
                    # The shapes whose header is reached in the SOURCE root, through each
                    # build directory's own link back to it.
                    if ($shape -in 'through', 'subthrough', 'selfthrough', 'srcself', 'dotdotsub') {
                        New-Item -ItemType Directory -Force -Path (Join-Path $checkout "src\gen") | Out-Null
                        Set-Content -Path (Join-Path $checkout "src\gen\g.h") -Value $header
                    }
                    # The dot shapes climb back out of a plain directory beside the junctions.
                    New-Item -ItemType Directory -Force -Path (Join-Path $case "jx\zz") | Out-Null
                    $dirs = foreach ($n in 1, 2) {
                        $build = Join-Path $checkout "b$n"
                        New-Item -ItemType Directory -Force -Path (Join-Path $build "x\y\gen") | Out-Null
                        Set-Content -Path (Join-Path $build "x\y\gen\g.h") -Value $header
                        $outer = Join-Path $case "jx\L$n"
                        # `alias` is the spelling the object must name for THIS build
                        # directory, and must not name for the other.
                        switch ($shape) {
                            'flat' {
                                & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\x\y\gen" }
                            }
                            'nested' {
                                & $junction "$build\genlink" "$build\x\y\gen"; & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\genlink" }
                            }
                            'through' {
                                & $junction "$build\srclink" $checkout; & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\srclink\src\gen" }
                            }
                            'subthrough' {
                                & $junction "$build\x\srclink" $checkout; & $junction $outer "$build\x"
                                @{ long = $build; alias = $outer; include = "$outer\srclink\src\gen" }
                            }
                            # The reviewer's twists (jx2.ps1): the build tree reached twice ...
                            'selfbuild' {
                                & $junction "$build\self" $build; & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\self\x\y\gen" }
                            }
                            # ... twice and then through into the source root ...
                            'selfthrough' {
                                & $junction "$build\self" $build; & $junction "$build\srclink" $checkout; & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\self\srclink\src\gen" }
                            }
                            # ... through, then the source root twice (one `srcself` shared by
                            # both build directories) ...
                            'srcself' {
                                & $junction "$checkout\srcself" $checkout; & $junction "$build\srclink" $checkout; & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$outer\srclink\srcself\src\gen" }
                            }
                            # ... and the reverse: one shared alias of the SOURCE root, through a
                            # per-build link to build directory n.
                            'revthrough' {
                                $shared = Join-Path $case "jx\S"
                                & $junction $shared $checkout; & $junction "$checkout\bl$n" $build
                                @{ long = $build; alias = "$shared\bl$n"; include = "$shared\bl$n\x\y\gen" }
                            }
                            # rev-filemacro's dot segments. With no debug flag BOTH compilers
                            # write the `-I` spelling VERBATIM into the object, `.` and `..` kept
                            # (measured, no launcher), so that is the spelling each object must
                            # name, and must not name for the other build directory.
                            # Measured before the fix: clang-cl's build directory 2 HIT and
                            # printed `...\jx\zz\..\L1\x\y\gen\g.h`.
                            'dotdot' {
                                & $junction $outer $build
                                @{ long = $build; alias = "$case\jx\zz\..\L$n"; include = "$case\jx\zz\..\L$n\x\y\gen" }
                            }
                            'dotseg' {
                                & $junction $outer $build
                                @{ long = $build; alias = "$case\jx\.\L$n"; include = "$case\jx\.\L$n\x\y\gen" }
                            }
                            # ... dotdot under `cl /FC`, which writes the COLLAPSED spelling
                            # into the object, so it must name `jx\L<n>` ...
                            'dotdotfc' {
                                & $junction $outer $build
                                @{ long = $build; alias = $outer; include = "$case\jx\zz\..\L$n\x\y\gen" }
                            }
                            # ... and the same through a subdirectory back into the source root.
                            'dotdotsub' {
                                & $junction "$build\x\srclink" $checkout; & $junction $outer "$build\x"
                                @{ long = $build; alias = "$case\jx\zz\..\L$n"; include = "$case\jx\zz\..\L$n\srclink\src\gen" }
                            }
                        }
                    }
                    $compile = {
                        param($dir)
                        $compileArgs = @($cc, "/nologo", "/c", "/showIncludes") + $extra +
                                       @("-I$($dir.include)", "/Fo$($dir.long)\u.obj", (Join-Path $checkout "src\u.cpp"))
                        Invoke-LauncherIn $dir.long $checkout $dir.long $compileArgs
                    }
                    $names = {
                        param([string]$obj, [string]$dir)
                        if (-not (Test-Path $obj)) { return $false }
                        $text = [System.Text.Encoding]::Latin1.GetString([System.IO.File]::ReadAllBytes($obj)).ToLowerInvariant()
                        return $text.Contains($dir.ToLowerInvariant())
                    }
                    if ($direct -eq 'off') { $env:FASTCACHE_NO_DIRECT = "1" }
                    try {
                        $r1 = & $compile $dirs[0]
                        $r2 = & $compile $dirs[1]
                        Remove-Item (Join-Path $dirs[0].long "u.obj") -Force -ErrorAction SilentlyContinue
                        $r1again = & $compile $dirs[0]
                    } finally {
                        Remove-Item Env:\FASTCACHE_NO_DIRECT -ErrorAction SilentlyContinue
                    }
                    $obj1 = Join-Path $dirs[0].long "u.obj"; $obj2 = Join-Path $dirs[1].long "u.obj"
                    # The alias with its trailing separator, so `jx\L1` does not also match `jx\L1x`.
                    $oneNamesOne = & $names $obj1 ($dirs[0].alias + "\")
                    $twoNamesOne = & $names $obj2 ($dirs[0].alias + "\")
                    $twoNamesTwo = & $names $obj2 ($dirs[1].alias + "\")
                    $o2 = Get-BoundOutcome $r2.stderr
                    $o1again = Get-BoundOutcome $r1again.stderr
                    # The control first: the object names the JUNCTION spelling, or this leg
                    # exercises nothing the root spellings did not already catch.
                    $ok = $r1.code -eq 0 -and $r2.code -eq 0 -and $r1again.code -eq 0 -and $oneNamesOne `
                          -and -not $twoNamesOne -and $twoNamesTwo -and $o2 -ne 'HIT' -and $o1again -eq 'BOUND-HIT'
                    if ($ok) {
                        Write-Host "  $shape, direct $direct : b2=$o2 b1-again=$o1again, b2 names its own junction: OK ($cc)" -ForegroundColor Green
                    } else {
                        Write-Host ("  JUNCTION FAIL ($cc, $shape, direct $direct): b2=$o2 b1-again=$o1again b1-names-L1=$oneNamesOne " +
                                    "b2-names-L1=$twoNamesOne b2-names-L2=$twoNamesTwo codes=$($r1.code)/$($r2.code)/$($r1again.code)") -ForegroundColor Red
                        foreach ($leg in @(@{n="b1"; r=$r1}, @{n="b2"; r=$r2}, @{n="b1 again"; r=$r1again})) {
                            Write-Host "  --- $($leg.n) ---" -ForegroundColor Yellow
                            Write-Host $leg.r.stderr
                        }
                        $exit = 1
                    }
                } finally {
                    # Each junction as a LINK first: removing the tree must never walk into a target.
                    foreach ($link in $links) { [System.IO.Directory]::Delete($link, $false) }
                    Remove-Item -Recurse -Force $case -ErrorAction SilentlyContinue
                }
            }
        }

        Write-Host "=== dead peers: a cache that cannot answer leaves the compile to go on ($cc) ==="
        $addrBefore = $env:FASTCACHE_ADDR
        if (-not (Test-DeadPeers $cc)) { $exit = 1 }
        # Asserted, not assumed: every case after this one reads the address from the environment.
        if ($env:FASTCACHE_ADDR -ne $addrBefore) {
            Write-Host "  DEAD-PEER FAIL ($cc): FASTCACHE_ADDR was left at '$($env:FASTCACHE_ADDR)', not restored to '$addrBefore'" -ForegroundColor Red
            $exit = 1
        }
    }

    # --- CLI surface --------------------------------------------------------
    # The help text must describe the flags the binary actually accepts. This
    # repeats the unit-level guard against the shipped launcher, and it is the
    # only place the Windows-only "/?" spelling gets exercised.
    $help = Use-E2ELauncherState $launcherState { & $Launcher --help | Out-String }
    foreach ($flag in @('--show-stats','-s','--zero-stats','-z','--help','-h','/?','--version','--prefetch-group')) {
        if ($help -notmatch [regex]::Escape($flag)) {
            Write-Host "  HELP DRIFT: --help does not document $flag" -ForegroundColor Red
            $exit = 1
        }
    }

    Use-E2ELauncherState $launcherState { & $Launcher /? | Out-Null }
    if ($LASTEXITCODE -ne 0) { Write-Host "  '/?' did not print help" -ForegroundColor Red; $exit = 1 }

    Use-E2ELauncherState $launcherState { & $Launcher -s | Out-Null }
    if ($LASTEXITCODE -ne 0) { Write-Host "  '-s' returned non-zero" -ForegroundColor Red; $exit = 1 }

    # Retired spellings must be diagnosed (exit 2), not spawned as a compiler.
    Use-E2ELauncherState $launcherState { & $Launcher --stats 2>&1 | Out-Null }
    if ($LASTEXITCODE -ne 2) {
        Write-Host "  retired --stats should exit 2, got $LASTEXITCODE" -ForegroundColor Red
        $exit = 1
    }

    # The positive control, read BEFORE `-z` clears it: this run's log holds records.
    if ($ranAnyCompiler) {
        $recorded = Get-E2ELauncherStateRecordCount $launcherState
        if ($recorded -lt 1) {
            Write-Host "  the launcher recorded nothing in this run's state log ($($launcherState.Log))" -ForegroundColor Red
            $exit = 1
        } else {
            Write-Host "  this run's compiles were recorded in its own state log: $recorded record(s)" -ForegroundColor Green
        }
    }

    Use-E2ELauncherState $launcherState { & $Launcher -z | Out-Null }
    if ($LASTEXITCODE -ne 0) { Write-Host "  '-z' returned non-zero" -ForegroundColor Red; $exit = 1 }
    # And `-z` cleared THIS run's log -- the operation that deleted a developer's.
    if ($ranAnyCompiler -and (Test-Path $launcherState.Log)) {
        Write-Host "  '-z' left this run's state log in place ($($launcherState.Log)), so it cleared some other one" -ForegroundColor Red
        $exit = 1
    }

    if ($exit -eq 0) { Write-Host "  CLI surface matches --help: OK" -ForegroundColor Green }
}
finally {
    if ($server) { $server | Stop-Process -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $DeepTemp,$ShallowTemp,$MoveTemp,$EditTemp,$AliasTemp,$BoundTemp -ErrorAction SilentlyContinue
    Remove-Item Env:\FASTCACHE_ADDR,Env:\FASTCACHE_SOURCE_DIR,Env:\FASTCACHE_BINARY_DIR,Env:\FASTCACHE_VERBOSE -ErrorAction SilentlyContinue
    # What the run did to the caller's logs, reported on EVERY way out -- a failure path is
    # where a leak shows -- and a failure whatever else happened.
    $callerDamage = @(Exit-E2ELauncherState $launcherState)
    if ($callerDamage.Count) { $callerDamage | ForEach-Object { Write-Host "launcher E2E FAILED: $_" }; exit 1 }
}

if (-not $ranAnyCompiler) {
    # No MSVC-style compiler on PATH: nothing was actually verified, so report a
    # skip rather than a pass. A silent success here would hide a broken build.
    Write-Host "no cl/clang-cl on PATH; skipping"
    exit $SKIP
}

if ($exit -eq 0) { Write-Host "ALL LAUNCHER E2E CHECKS PASSED" -ForegroundColor Green }
else { Write-Host "SOME LAUNCHER E2E CHECKS FAILED" -ForegroundColor Red }
exit $exit
