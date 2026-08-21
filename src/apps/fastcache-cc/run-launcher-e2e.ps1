# SPDX-License-Identifier: Apache-2.0
#
# End-to-end validation of the fastcache-cc launcher with REAL compilations.
#
# Proves the launcher behaves as a drop-in sccache replacement:
#   1. First compile of a file is a MISS (real compiler runs, result stored).
#   2. Second compile of the same content is a HIT (served from cache, correct
#      object produced) — even after the object is deleted.
#   3. Cross-depth: content compiled/stored with a DEEP srcroot is a HIT when
#      compiled from a SHALLOW srcroot (different checkout), with showIncludes
#      localized so the deps resolve.
#   4. An EDITED source is a MISS, and yields a different object. The preprocessed
#      text is the only key input carrying the source's content, so a probe that
#      captures none of it answers an edit with the previous revision's object — a
#      wrong build, silently, every time. Checked with direct mode off, because the
#      manifest hashes the source's own bytes and would otherwise mask it. This is
#      the property `/EP` plus `/P` broke: the pair writes the preprocessed text to
#      a FILE, leaving the launcher hashing an empty stdout.
#   5. Moved header: a header that moves with its contents unchanged is a MISS by
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
    [string]$DeepTemp    = "$env:TEMP/cc-l-deep",
    [string]$ShallowTemp = "$env:TEMP/cc-l-shallow",
    [string]$MoveTemp    = "$env:TEMP/cc-l-move",
    [string]$EditTemp    = "$env:TEMP/cc-l-edit"
)

$ErrorActionPreference = "Stop"
$exit = 0
# CTest's SKIP_RETURN_CODE. A missing binary or compiler is a missing runtime
# prerequisite, not a failure, so it must be distinguishable from a real fault.
$SKIP = 77
$ranAnyCompiler = $false

if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }

# Start-Process resolves a relative -FilePath against the PROCESS working
# directory, not PowerShell's, so a caller passing "out/build/..." would get a
# spurious "file not found". Resolve both up front.
$Fastcached = (Resolve-Path $Fastcached).Path
$Launcher   = (Resolve-Path $Launcher).Path

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

function New-Tree([string]$root) {
    $src = Join-Path $root "src"
    $inc = Join-Path $src "inc"
    New-Item -ItemType Directory -Force -Path $inc | Out-Null
    Set-Content -Path (Join-Path $inc "h1.h") -Value "#pragma once`nint one();`n"
    Set-Content -Path (Join-Path $src "u.cpp") -Value "#include `"inc/h1.h`"`nint g(){return one();}`n"
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
        $src = New-Tree $ShallowTemp
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
        Remove-Item -Recurse -Force $DeepTemp -ErrorAction SilentlyContinue
        $deepSrc = New-Tree (Join-Path $DeepTemp "a/b/c/d")
        $deepBuild = Join-Path (Split-Path $deepSrc -Parent) "build"; New-Item -ItemType Directory -Force $deepBuild | Out-Null
        $rDeep = Invoke-Launcher $cc $deepSrc $deepBuild (Join-Path $deepBuild "u.obj")
        $oDeep = Get-Outcome $rDeep.stderr

        # New shallow checkout of identical content.
        Remove-Item -Recurse -Force $ShallowTemp -ErrorAction SilentlyContinue
        $src2 = New-Tree $ShallowTemp
        $build2 = Join-Path $ShallowTemp "build"; New-Item -ItemType Directory -Force $build2 | Out-Null
        $obj2 = Join-Path $build2 "u.obj"
        $rShallow = Invoke-Launcher $cc $src2 $build2 $obj2
        $oShallow = Get-Outcome $rShallow.stderr

        if ($oDeep -eq "MISS" -and $oShallow -eq "HIT" -and (Test-Path $obj2)) {
            Write-Host "  deep store -> shallow HIT, object produced: OK ($cc)" -ForegroundColor Green
        } else {
            Write-Host "  CROSS-DEPTH FAIL ($cc): deep=$oDeep shallow=$oShallow obj=$(Test-Path $obj2)" -ForegroundColor Red
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
        $oBefore = "UNKNOWN"; $oMoved = "UNKNOWN"; $oBack = "UNKNOWN"; $staleHit = $false
        $env:FASTCACHE_NO_DIRECT = "1"
        try {
            $oBefore = Get-Outcome (Invoke-Launcher $cc $moveSrc $moveBuild $moveObj).stderr

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
