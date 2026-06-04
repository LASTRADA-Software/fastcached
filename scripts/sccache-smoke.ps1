# SPDX-License-Identifier: Apache-2.0
#
# sccache smoke test (Windows / PowerShell). Mirrors scripts/sccache-smoke.sh:
# start fastcached, point sccache at it over the chosen wire protocol, compile
# a one-file program twice, and assert the second compile is served from cache.
#
# Usage:
#   pwsh -File sccache-smoke.ps1 --fastcached <path> [--protocol memcached|redis]
#        [--port <n>] [--compiler <cxx>]
#
# Exit codes: 0 = cache hit observed; 1 = ran but no cache hit; 77 = a runtime
# prerequisite (sccache / fastcached / compiler) was missing — treated as a
# skip by CTest's SKIP_RETURN_CODE.
$ErrorActionPreference = 'Stop'

$fastcached = ''
$protocol = 'memcached'
$port = '11611'
$compiler = if ($env:CXX) { $env:CXX } else { 'cl' }

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        '--fastcached' { $fastcached = $args[++$i] }
        '--protocol' { $protocol = $args[++$i] }
        '--port' { $port = $args[++$i] }
        '--compiler' { $compiler = $args[++$i] }
        default { Write-Host "unknown argument: $($args[$i])"; exit 2 }
    }
}

$SKIP = 77
function Test-Have($name) { $null -ne (Get-Command $name -ErrorAction SilentlyContinue) }

if (-not (Test-Have 'sccache')) { Write-Host 'sccache not found; skipping'; exit $SKIP }
if (-not $fastcached -or -not (Test-Path $fastcached)) { Write-Host "fastcached not found: '$fastcached'; skipping"; exit $SKIP }
if (-not (Test-Have $compiler)) { Write-Host "compiler not found: '$compiler'; skipping"; exit $SKIP }

switch ($protocol) {
    'memcached' { $env:SCCACHE_MEMCACHED = "tcp://127.0.0.1:$port" }
    'redis' { $env:SCCACHE_REDIS = "redis://127.0.0.1:$port" }
    default { Write-Host "unknown protocol: '$protocol'"; exit 2 }
}
$env:SCCACHE_NO_DAEMON = '0'

$workdir = Join-Path ([System.IO.Path]::GetTempPath()) ('sccache-smoke-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $workdir | Out-Null
$src = Join-Path $workdir 'hello.cpp'
$obj = Join-Path $workdir 'hello.obj'
@'
#include <string>
int main() { return static_cast<int>(std::string{"hi"}.size()); }
'@ | Set-Content -Path $src -Encoding ASCII

$server = $null
function Invoke-Cleanup {
    & sccache --stop-server *> $null
    if ($script:server -and -not $script:server.HasExited) {
        Stop-Process -Id $script:server.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -Recurse -Force $workdir -ErrorAction SilentlyContinue
}

# Compiler family: cl / clang-cl take MSVC switches, everything else GCC-style.
$leaf = [System.IO.Path]::GetFileNameWithoutExtension($compiler).ToLowerInvariant()
if ($leaf -eq 'cl' -or $leaf -eq 'clang-cl') {
    $compileArgs = @($compiler, '/nologo', '/std:c++latest', '/EHsc', '/c', $src, "/Fo$obj")
}
else {
    $compileArgs = @($compiler, '-std=c++23', '-c', $src, '-o', $obj)
}

$server = Start-Process -FilePath $fastcached -ArgumentList "--port=$port", '--log-level=info' -PassThru -NoNewWindow
Start-Sleep -Seconds 1

& sccache --stop-server *> $null
& sccache --start-server | Out-Null
& sccache --zero-stats | Out-Null

# First compile: cache miss; second: must hit.
& sccache @compileArgs
if ($LASTEXITCODE -ne 0) { Write-Host "first compile failed ($LASTEXITCODE)"; Invoke-Cleanup; exit 1 }
Remove-Item -Force $obj -ErrorAction SilentlyContinue
& sccache @compileArgs
if ($LASTEXITCODE -ne 0) { Write-Host "second compile failed ($LASTEXITCODE)"; Invoke-Cleanup; exit 1 }

$stats = (& sccache --show-stats | Out-String)
Write-Host $stats
Invoke-Cleanup
if ($stats -match 'Cache hits\s+[1-9]') {
    Write-Host "sccache smoke ($protocol) OK: cache hit observed"
    exit 0
}
Write-Host "sccache smoke ($protocol) FAILED: no cache hit"
exit 1
