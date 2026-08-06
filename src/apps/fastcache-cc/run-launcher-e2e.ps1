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
#
# Output is generic status only; nothing project-private is read or emitted,
# nothing is committed.

[CmdletBinding()]
param(
    [string]$Fastcached  = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Launcher    = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcache-cc.exe",
    [int]$Port           = 21714,
    [string]$DeepTemp    = "$env:TEMP/cc-l-deep",
    [string]$ShallowTemp = "$env:TEMP/cc-l-shallow"
)

$ErrorActionPreference = "Stop"
$exit = 0
# CTest's SKIP_RETURN_CODE. A missing binary or compiler is a missing runtime
# prerequisite, not a failure, so it must be distinguishable from a real fault.
$SKIP = 77
$ranAnyCompiler = $false

if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Launcher))   { Write-Host "fastcache-cc not found: $Launcher; skipping"; exit $SKIP }

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

# Run the launcher once; return @{ code; stderr } and capture whether it was a
# HIT or MISS from the verbose trace.
function Invoke-Launcher([string]$compiler, [string]$srcRoot, [string]$buildTree, [string]$obj) {
    $env:FASTCACHE_ADDR      = "127.0.0.1:$Port"
    $env:FASTCACHE_SRCROOT   = $srcRoot
    $env:FASTCACHE_BUILDTREE = $buildTree
    $env:FASTCACHE_VERBOSE   = "1"
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
    }
}
finally {
    $server | Stop-Process -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $DeepTemp,$ShallowTemp -ErrorAction SilentlyContinue
    Remove-Item Env:\FASTCACHE_ADDR,Env:\FASTCACHE_SRCROOT,Env:\FASTCACHE_BUILDTREE,Env:\FASTCACHE_VERBOSE -ErrorAction SilentlyContinue
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
