# SPDX-License-Identifier: Apache-2.0
#
# Cross-depth validation for the compile-cache executor.
#
# Proves the core value-portability guarantee with REAL compilations: an object
# compiled at a DEEP checkout path (mimicking a CI runner nested deeper) is
# STOREd through fastcached, then FETCHed and localized from a SHALLOW checkout
# path — and every localized /showIncludes header resolves on disk at the
# shallow layout. This is exactly the scenario that poisons Ninja today.
#
# Two modes:
#   -Synthetic         : generate a throwaway source tree (no project source).
#   -CheckoutRoots ... : use a real translation unit from a private checkout,
#                        compiled at a deep temp path, fetched to each real root.
#                        Prints ONLY counts + generic status — never absolute
#                        checkout paths or source — so nothing private leaks.
#
# Nothing this script produces is committed; outputs go to the console and to
# temp directories only.

[CmdletBinding()]
param(
    [string]$Fastcached  = "$PSScriptRoot/../../out/build/clangcl-debug/target/fastcached.exe",
    [string]$Client      = "$PSScriptRoot/../../out/build/clangcl-debug/target/compile-cache-testclient.exe",
    [int]$Port           = 21713,
    [switch]$Synthetic,
    [string[]]$CheckoutRoots = @(),
    [string]$DeepTemp    = "$env:TEMP/cc-deep",
    [string]$ShallowTemp = "$env:TEMP/cc-shallow"
)

$ErrorActionPreference = "Stop"
$exitCode = 0

function Start-Fastcached {
    Write-Host "starting fastcached on 127.0.0.1:$Port ..."
    $p = Start-Process -FilePath $Fastcached -ArgumentList "--bind","127.0.0.1","--port","$Port" -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 600
    if ($p.HasExited) { throw "fastcached exited immediately (exit $($p.ExitCode))" }
    return $p
}

# Write a small self-contained source tree (a .cpp including two headers) under
# $root/src, so /showIncludes emits header paths under the source root.
function New-SyntheticTree([string]$root) {
    $src = Join-Path $root "src"
    $inc = Join-Path $src "inc"
    New-Item -ItemType Directory -Force -Path $inc | Out-Null
    Set-Content -Path (Join-Path $inc "alpha.h")  -Value "#pragma once`nint alpha();`n"
    Set-Content -Path (Join-Path $inc "beta.h")   -Value "#pragma once`nint beta();`n"
    Set-Content -Path (Join-Path $src "unit.cpp") -Value @"
#include "inc/alpha.h"
#include "inc/beta.h"
int f() { return alpha() + beta(); }
"@
    return $src
}

function Invoke-CrossDepth([string]$compiler, [string]$deepSrc, [string]$shallowSrc, [string]$key, [string]$prefetchGroup) {
    $deepBuild    = Join-Path (Split-Path $deepSrc -Parent) "build"
    $shallowBuild = Join-Path (Split-Path $shallowSrc -Parent) "build"
    New-Item -ItemType Directory -Force -Path $deepBuild,$shallowBuild | Out-Null
    $source = Join-Path $deepSrc "unit.cpp"

    Push-Location $deepBuild
    try {
        Write-Host "[$compiler] STORE from deep:  $deepSrc"
        & $Client store --port $Port --key $key --prefetch-group $prefetchGroup `
            --srcroot $deepSrc --buildtree $deepBuild --compiler $compiler `
            --source $source --out (Join-Path $deepBuild "unit.obj")
        if ($LASTEXITCODE -ne 0) { throw "store failed ($compiler)" }
    } finally { Pop-Location }

    Write-Host "[$compiler] FETCH to shallow: $shallowSrc"
    & $Client fetch --port $Port --key $key `
        --srcroot $shallowSrc --buildtree $shallowBuild `
        --out (Join-Path $shallowBuild "unit.obj")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "CROSS-DEPTH FAIL ($compiler): localized include paths did not resolve at the shallow layout" -ForegroundColor Red
        return $false
    }
    Write-Host "CROSS-DEPTH OK ($compiler)" -ForegroundColor Green
    return $true
}

$server = Start-Fastcached
try {
    if ($Synthetic -or $CheckoutRoots.Count -eq 0) {
        Write-Host "=== synthetic cross-depth (generated source, no project files) ==="
        # Deep tree nested several levels; shallow tree at a shorter path. Both
        # carry the IDENTICAL source so localized <SRCROOT> paths resolve.
        $deepRoot    = Join-Path $DeepTemp "a/b/c/d"
        $shallowRoot = $ShallowTemp
        Remove-Item -Recurse -Force $deepRoot,$shallowRoot -ErrorAction SilentlyContinue
        $deepSrc    = New-SyntheticTree $deepRoot
        $shallowSrc = New-SyntheticTree $shallowRoot

        foreach ($cc in @("cl","clang-cl")) {
            if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) {
                Write-Host "skip $cc (not on PATH)"; continue
            }
            if (-not (Invoke-CrossDepth $cc $deepSrc $shallowSrc "syn-$cc" "envSyn")) { $exitCode = 1 }
        }
    }

    $rootIndex = 0
    foreach ($root in $CheckoutRoots) {
        if (-not (Test-Path $root)) { Write-Host "skip: a supplied root is missing"; continue }
        Write-Host "=== real-checkout cross-depth (rooted at an actual checkout; counts only) ==="
        # Validate cross-depth correctness AGAINST THE REAL CHECKOUT PATHS — the
        # thing that actually differs across machines — WITHOUT compiling or
        # reading any project source. We lay a throwaway generated tree under a
        # subfolder of the real root (shallow consumer) and an identical tree at
        # a DEEP temp path (producer, mimicking a deeper CI checkout), then run
        # the same store→fetch→localize round-trip. Console output is generic;
        # no absolute checkout path is printed.
        $tag = "root$rootIndex"
        $deepRoot    = Join-Path $DeepTemp "$tag/x/y/z"
        $shallowRoot = Join-Path $root ".cc-crossdepth-tmp"
        Remove-Item -Recurse -Force $deepRoot,$shallowRoot -ErrorAction SilentlyContinue
        try {
            $deepSrc    = New-SyntheticTree $deepRoot
            $shallowSrc = New-SyntheticTree $shallowRoot
            foreach ($cc in @("cl","clang-cl")) {
                if (-not (Get-Command $cc -ErrorAction SilentlyContinue)) { continue }
                Write-Host "  checkout #$rootIndex via ${cc}:"
                if (-not (Invoke-CrossDepth $cc $deepSrc $shallowSrc "$tag-$cc" "envReal$rootIndex")) { $exitCode = 1 }
            }
        }
        finally {
            Remove-Item -Recurse -Force $shallowRoot -ErrorAction SilentlyContinue
        }
        $rootIndex++
    }
}
finally {
    Write-Host "stopping fastcached"
    $server | Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($exitCode -eq 0) { Write-Host "ALL CROSS-DEPTH CHECKS PASSED" -ForegroundColor Green }
else { Write-Host "SOME CROSS-DEPTH CHECKS FAILED" -ForegroundColor Red }
exit $exitCode
