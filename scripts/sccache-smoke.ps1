# SPDX-License-Identifier: Apache-2.0
#
# sccache smoke test (Windows / PowerShell). Mirrors scripts/sccache-smoke.sh:
# start fastcached, point sccache at it over the chosen wire protocol, compile
# a one-file program twice, and assert the second compile is served from cache.
#
# Usage:
#   pwsh -File sccache-smoke.ps1 --fastcached <path> [--protocol memcached|redis]
#        [--port <n>] [--compiler <cxx>] [--expect-flavor <name>]
#
# Exit codes: 0 = every assertion held; 1 = ran and something was wrong; 77 = a
# runtime prerequisite (sccache / fastcached / compiler / a backend that is not
# compiled into this sccache) was missing.
#
# 77 is a SKIP under CTest (`SKIP_RETURN_CODE`) and would be a FAILURE from a
# GitHub Actions `run:` step, which is not told what it means. No CI job runs this
# driver today; the note is here so that adding one is a decision rather than a
# surprise.
#
# ## What this asserts, and why one assertion was never enough
#
# `Cache hits >= 1` alone is satisfied by sccache's own LOCAL DISK cache, so the
# fixture passed with no fastcached in the picture at all, for as long as it
# existed (#1318). Three questions, because each is satisfied by the failing state
# of the next:
#
#   1. is the backend even IN this sccache        -- `Enabled features:`
#   2. did sccache USE it, or fall back silently  -- `Cache location`
#   3. WHICH DIALECT did it speak                 -- the daemon's own log
#
# (3) matters because `Cache location` reports the backend TYPE: a binary-speaking
# sccache reports `memcached, name: memcached, prefix: /` exactly as a text-speaking
# one does, so without it a leg named for a dialect could exercise the other and go
# on claiming coverage.
#
# STILL NOT DONE HERE, and tracked as #183: this driver fixes its port and waits
# for readiness with a flat `Start-Sleep`. The POSIX half draws a port per run and
# waits on the listener; this one does not yet. The bounded wait added below is for
# the STORE, which the new assertions need, and is not that fix.
$ErrorActionPreference = 'Stop'

$fastcached = ''
$protocol = 'memcached'
$port = '11611'
$compiler = if ($env:CXX) { $env:CXX } else { 'cl' }
# Exact dialect this caller expects. Optional: given, it must match exactly (the
# caller pinned the sccache and therefore knows); omitted, only the protocol's
# family is required and the observed dialect is printed. Both still fail when the
# daemon saw nothing at all, which is the case this fixture never covered.
$expectFlavor = ''

for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        '--fastcached' { $fastcached = $args[++$i] }
        '--protocol' { $protocol = $args[++$i] }
        '--port' { $port = $args[++$i] }
        '--compiler' { $compiler = $args[++$i] }
        '--expect-flavor' { $expectFlavor = $args[++$i] }
        default { Write-Host "unknown argument: $($args[$i])"; exit 2 }
    }
}

$SKIP = 77
function Test-Have($name) { $null -ne (Get-Command $name -ErrorAction SilentlyContinue) }

if (-not (Test-Have 'sccache')) { Write-Host 'sccache not found; skipping'; exit $SKIP }
if (-not $fastcached -or -not (Test-Path $fastcached)) { Write-Host "fastcached not found: '$fastcached'; skipping"; exit $SKIP }
if (-not (Test-Have $compiler)) { Write-Host "compiler not found: '$compiler'; skipping"; exit $SKIP }

# One row per protocol and every column this fixture needs about it, so adding a
# backend is a row and cannot be half-added.
$BackendTable = @{
    'memcached' = @{
        Env      = 'SCCACHE_MEMCACHED'
        Scheme   = 'tcp'
        Feature  = 'Memcached'
        Location = 'memcached'
        Flavors  = @('memcached-text', 'memcached-binary')
    }
    'redis'     = @{
        Env      = 'SCCACHE_REDIS'
        Scheme   = 'redis'
        Feature  = 'Redis'
        Location = 'redis'
        Flavors  = @('redis-resp')
    }
}

$backend = $BackendTable[$protocol]
if (-not $backend) { Write-Host "unknown protocol: '$protocol' (no row in BackendTable)"; exit 2 }

if ($expectFlavor -and ($backend.Flavors -notcontains $expectFlavor)) {
    Write-Host "--expect-flavor '$expectFlavor' is not a flavour the '$protocol' backend can produce"
    Write-Host "  this protocol's flavours: $($backend.Flavors -join ', ')"
    exit 2
}

# Is the backend COMPILED IN? Asked of `sccache --help`, which carries an
# `Enabled features:` block, and asked BEFORE anything starts -- a backend that is
# absent makes every later assertion a statement about sccache's local disk cache.
#
# This replaces a guard that could never fire: the old one skipped when
# `--start-server` failed with "Cache type not supported with current feature
# configuration", and a feature-less sccache does not refuse, it DEGRADES. Measured
# on the POSIX side, where Ubuntu ships 0.7.7 with `Memcached: false` while the
# official 0.7.7 binary reports `true` -- same version, different build.
$help = (& sccache --help 2>&1 | Out-String)
if ($help -notmatch "(?m)^\s+$($backend.Feature):\s+true\s*$") {
    Write-Host "this sccache is built without the $($backend.Feature) backend; skipping"
    $featureBlock = [regex]::Match($help, '(?s)Enabled features:.*?(\r?\n\r?\n|$)').Value
    if ($featureBlock) { Write-Host $featureBlock }
    Write-Host '  a distribution package may omit it where the official release binary carries it,'
    Write-Host '  so this is a question of WHICH sccache, not of configuration.'
    exit $SKIP
}

Set-Item -Path "env:$($backend.Env)" -Value "$($backend.Scheme)://127.0.0.1:$port"
$env:SCCACHE_NO_DAEMON = '0'

$workdir = Join-Path ([System.IO.Path]::GetTempPath()) ('sccache-smoke-' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $workdir | Out-Null
$src = Join-Path $workdir 'hello.cpp'
$obj = Join-Path $workdir 'hello.obj'
# Start-Process cannot send stdout and stderr to one file, so they are separate
# and read back together.
$daemonOut = Join-Path $workdir 'fastcached.out.log'
$daemonErr = Join-Path $workdir 'fastcached.err.log'
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

function Get-DaemonLog {
    $text = ''
    foreach ($f in @($daemonOut, $daemonErr)) {
        if (Test-Path $f) { $text += (Get-Content -Raw -ErrorAction SilentlyContinue $f) }
    }
    return $text
}

function Stop-Smoke($reason, $detail) {
    Write-Host "sccache smoke ($protocol) FAILED: $reason"
    foreach ($line in $detail) { Write-Host "  $line" }
    Invoke-Cleanup
    exit 1
}

# Compiler family: cl / clang-cl take MSVC switches, everything else GCC-style.
$leaf = [System.IO.Path]::GetFileNameWithoutExtension($compiler).ToLowerInvariant()
if ($leaf -eq 'cl' -or $leaf -eq 'clang-cl') {
    $compileArgs = @($compiler, '/nologo', '/std:c++latest', '/EHsc', '/c', $src, "/Fo$obj")
}
else {
    $compileArgs = @($compiler, '-std=c++23', '-c', $src, '-o', $obj)
}

# `--log-level=trace --log-everything` is what makes the DIALECT observable, and
# both are required: Connection.cpp logs `connection accepted (<flavour>)` at Trace,
# gated on logEverything, rendering `memcached-text` / `memcached-binary` /
# `redis-resp`. At --log-level=info that line does not appear at all, which is why
# nothing could tell a text-speaking client from a binary-speaking one. No
# production change was needed: the daemon has always known and has always been
# able to say so. Nothing asked.
$server = Start-Process -FilePath $fastcached `
    -ArgumentList "--port=$port", '--log-level=trace', '--log-everything' `
    -PassThru -NoNewWindow -RedirectStandardOutput $daemonOut -RedirectStandardError $daemonErr
Start-Sleep -Seconds 1

& sccache --stop-server *> $null
& sccache --start-server | Out-Null
& sccache --zero-stats | Out-Null

# First compile: cache miss; it is what populates fastcached.
& sccache @compileArgs
if ($LASTEXITCODE -ne 0) { Stop-Smoke "first compile failed ($LASTEXITCODE)" @() }
Remove-Item -Force $obj -ErrorAction SilentlyContinue

# DID SCCACHE USE THE BACKEND AT ALL? Asked here, between the compiles, and the
# position is the point: if sccache fell back to local disk the daemon will never
# see a store, so the wait below would expire and report a TIMEOUT -- true, and a
# diagnosis of the wrong thing. Asking first names the cause instead.
#
# Asserted POSITIVELY, by the configured backend's name. The tempting negative --
# "does not say Local disk" -- fails OPEN the day sccache gains a third fallback.
$firstStats = (& sccache --show-stats | Out-String)
$locationLine = ($firstStats -split "`r?`n" | Where-Object { $_ -match '^\s*Cache location' }) -join '; '
if ($locationLine -notmatch "^\s*Cache location\s+$([regex]::Escape($backend.Location))") {
    Stop-Smoke "sccache did not use the $protocol backend" @(
        "expected 'Cache location' to name '$($backend.Location)'.",
        "got: $(if ($locationLine) { $locationLine } else { '(no Cache location line at all)' })",
        'sccache falls back to its own local disk cache SILENTLY, and a hit from',
        'that cache satisfies the hit assertion with no fastcached involved (#1318).')
}

# WAIT FOR THE STORE TO LAND. sccache's write to a REMOTE backend is asynchronous:
# measured on the POSIX side with official sccache 0.7.7, both lookups reached the
# daemon BEFORE either store, so compiling twice back to back is a race and the
# fixture reports "no cache hit" against a daemon that is working perfectly.
# Measured without this wait: 1 pass in 15. With it: 15 in 15.
#
# Bounded, and it says what it waited for and what it cost. The marker excludes
# sccache's `.sccache_check` probe, which also stores -- an object key is sharded
# hex, the probe key is not.
$deadline = (Get-Date).AddSeconds(20)
$stored = $false
while ((Get-Date) -lt $deadline) {
    if ((Get-DaemonLog) -match 'storage: SET key=[0-9a-f]/') { $stored = $true; break }
    if ($server.HasExited) {
        Stop-Smoke 'fastcached exited while waiting for it to store the first object' @(
            "exit code: $($server.ExitCode)", '--- daemon log ---', (Get-DaemonLog))
    }
    Start-Sleep -Milliseconds 200
}
if (-not $stored) {
    Stop-Smoke 'timed out waiting for fastcached to store the first compile object' @(
        'waited 20s for a line matching: storage: SET key=[0-9a-f]/',
        'the daemon is alive, so this is a slow machine or a store that never arrived.')
}

# Second compile: must now be a cache hit.
& sccache @compileArgs
if ($LASTEXITCODE -ne 0) { Stop-Smoke "second compile failed ($LASTEXITCODE)" @() }

$stats = (& sccache --show-stats | Out-String)
Write-Host $stats

# 1. A hit happened at all -- necessary, and on its own satisfied by the local disk
#    cache with no daemon anywhere, which is why it is one of three.
if ($stats -notmatch 'Cache hits\s+[1-9]') {
    Stop-Smoke 'no cache hit' @('sccache reported no hit on the second compile.')
}

# 2. Re-read, because this is a different `--show-stats` invocation than the one
#    checked between the compiles and a backend that changed under us would
#    otherwise go unremarked.
if ($stats -notmatch "(?m)^\s*Cache location\s+$([regex]::Escape($backend.Location))") {
    Stop-Smoke "sccache stopped using the $protocol backend mid-run" @(
        "expected 'Cache location' to name '$($backend.Location)'.")
}

# 3. WHICH DIALECT the daemon was spoken to -- the daemon is the only thing that
#    knows, since `Cache location` reports the backend TYPE.
$observed = @([regex]::Matches((Get-DaemonLog), 'connection accepted \(([a-z-]+)\)') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)

if ($observed.Count -eq 0) {
    Stop-Smoke 'the daemon logged no accepted connection' @(
        "nothing matched 'connection accepted (<flavour>)' in the daemon log.",
        'that line is Trace and gated on --log-everything; both are passed above.',
        'an empty result here means sccache never reached this daemon.')
}

if ($expectFlavor) {
    if ($observed.Count -ne 1 -or $observed[0] -ne $expectFlavor) {
        Stop-Smoke 'the daemon was spoken to in the wrong dialect' @(
            "expected exactly '$expectFlavor', observed '$($observed -join ', ')'.",
            'this leg is NAMED for its dialect, so a different one passing every',
            'other assertion would be a false coverage claim (#1318).')
    }
}
else {
    foreach ($flavor in $observed) {
        if ($backend.Flavors -notcontains $flavor) {
            Stop-Smoke 'the daemon was spoken to in a foreign dialect' @(
                "observed '$flavor', which is not a '$protocol' flavour.",
                "this protocol's flavours: $($backend.Flavors -join ', ')")
        }
    }
}

Invoke-Cleanup
Write-Host "sccache smoke ($protocol) OK: cache hit observed, served by $($backend.Location), dialect $($observed -join ', ')"
exit 0
