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
#
# The POSIX counterpart (scripts/compile-cache-e2e.sh) additionally asserts that
# a hit restores the GNU depfile, localized to the consuming checkout. That has
# no analogue here: the MSVC drivers report dependencies inline via
# /showIncludes (asserted above) and set usesDepfile = false, so -MF is never
# parsed and no depfile region is ever stored for them.
#
# Output is generic status only; nothing project-private is read or emitted,
# nothing is committed.

[CmdletBinding()]
param(
    [string]$Fastcached  = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Launcher    = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcache-cc.exe",
    [int]$Port           = 21714,
    # Left empty on purpose: these default to a directory beside the build tree,
    # computed once the launcher path is resolved. See the note there for why not
    # %TEMP%. Passing one explicitly still works and is honoured verbatim.
    [string]$DeepTemp,
    [string]$ShallowTemp,
    [string]$MoveTemp,
    [string]$EditTemp
)

$ErrorActionPreference = "Stop"
$exit = 0

# CTest's SKIP_RETURN_CODE. A missing binary or compiler is a missing runtime
# prerequisite, not a failure, so it must be distinguishable from a real fault.
$SKIP = 77
$ranAnyCompiler = $false

if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }

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
# reported path(s) keyed"). That is a launcher limitation — issue #66, see
# AGENT.md — and not what these cases are here to measure.
#
# The build tree is the fix rather than expanding the short name, because there is
# no dependable way to expand one: Resolve-Path, Get-Item and
# [IO.Path]::GetFullPath all preserve it, and Scripting.FileSystemObject was tried
# and echoed it back unchanged. No checkout reached through a short name would
# have built here in the first place.

# Start-Process resolves a relative -FilePath against the PROCESS working
# directory, not PowerShell's, so a caller passing "out/build/..." would get a
# spurious "file not found". Resolve both up front.
$Fastcached = (Resolve-Path $Fastcached).Path
$Launcher   = (Resolve-Path $Launcher).Path

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

# Belt and braces, and it covers the caller too: a root passed explicitly is
# honoured verbatim, so `-MoveTemp scratch/move` would reintroduce exactly the
# split above. Rooting every one of them here means the cases cannot silently
# degrade into testing clang-cl only — the failure mode this guard exists for is
# a PASS on one driver and a meaningless comparison on the other.
foreach ($name in 'DeepTemp', 'ShallowTemp', 'MoveTemp', 'EditTemp') {
    $value = Get-Variable -Name $name -ValueOnly
    if (-not [System.IO.Path]::IsPathRooted($value)) {
        Set-Variable -Name $name -Value (Join-Path (Get-Location).Path $value)
    }
}

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

# Run the launcher once; return @{ code; stderr } and capture whether it was a
# HIT or MISS from the verbose trace.
function Invoke-Launcher([string]$compiler, [string]$srcRoot, [string]$buildTree, [string]$obj) {
    $env:FASTCACHE_ADDR       = "127.0.0.1:$Port"
    $env:FASTCACHE_SOURCE_DIR = $srcRoot
    $env:FASTCACHE_BINARY_DIR = $buildTree
    $env:FASTCACHE_VERBOSE    = "1"
    $source = Join-Path $srcRoot "u.cpp"
    $errFile = New-TemporaryFile
    $p = Start-Process -FilePath $Launcher `
        -ArgumentList $compiler,"/nologo","/c","/showIncludes","/Fo$obj",$source `
        -NoNewWindow -Wait -PassThru -RedirectStandardError $errFile
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

function Get-Outcome([string]$stderr) {
    if ($stderr -match "fastcache-cc: HIT")  { return "HIT" }
    if ($stderr -match "fastcache-cc: MISS") { return "MISS" }
    return "UNKNOWN"
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
    $p = Start-Process -FilePath $Launcher `
        -ArgumentList $compiler,"/nologo","/c","/Fo$obj",$source `
        -NoNewWindow -PassThru -RedirectStandardError $errFile
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

$server = Start-Fastcached
try {
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
    }

    # --- CLI surface --------------------------------------------------------
    # The help text must describe the flags the binary actually accepts. This
    # repeats the unit-level guard against the shipped launcher, and it is the
    # only place the Windows-only "/?" spelling gets exercised.
    $help = (& $Launcher --help | Out-String)
    foreach ($flag in @('--show-stats','-s','--zero-stats','-z','--help','-h','/?','--version','--prefetch-group')) {
        if ($help -notmatch [regex]::Escape($flag)) {
            Write-Host "  HELP DRIFT: --help does not document $flag" -ForegroundColor Red
            $exit = 1
        }
    }

    & $Launcher /? | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  '/?' did not print help" -ForegroundColor Red; $exit = 1 }

    & $Launcher -s | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  '-s' returned non-zero" -ForegroundColor Red; $exit = 1 }

    # Retired spellings must be diagnosed (exit 2), not spawned as a compiler.
    & $Launcher --stats 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 2) {
        Write-Host "  retired --stats should exit 2, got $LASTEXITCODE" -ForegroundColor Red
        $exit = 1
    }

    & $Launcher -z | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "  '-z' returned non-zero" -ForegroundColor Red; $exit = 1 }

    if ($exit -eq 0) { Write-Host "  CLI surface matches --help: OK" -ForegroundColor Green }
}
finally {
    $server | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $DeepTemp,$ShallowTemp,$MoveTemp,$EditTemp -ErrorAction SilentlyContinue
    Remove-Item Env:\FASTCACHE_ADDR,Env:\FASTCACHE_SOURCE_DIR,Env:\FASTCACHE_BINARY_DIR,Env:\FASTCACHE_VERBOSE -ErrorAction SilentlyContinue
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
