# SPDX-License-Identifier: Apache-2.0
#
# TLS smoke test (Windows). Start fastcached with TLS terminated using the
# checked-in self-signed cert, connect with a .NET SslStream (no external
# openssl needed on the client side), send a RESP PING, and assert the daemon
# answers +PONG over the encrypted channel.
#
# Usage:
#   tls-smoke.ps1 -Fastcached <path> -Cert <pem> -Key <pem> [-Port <n>]
#
# The port is DRAWN per run unless one is passed; a passed one is probed first and
# a leftover listener is refused by name. See scripts/lib/E2EPorts.psm1.
#
# Exit codes: 0 = +PONG over TLS; 1 = ran but no PONG; 77 = a runtime
# prerequisite (fastcached / cert) was missing — treated as a skip.
param(
    [string]$Fastcached = "",
    # Drawn per run, for the reason `.agent/rules/testing.md` gives: a fixed port
    # needs a reaper, and this file had neither. The constant it used to carry was
    # `11811` -- shared byte for byte with the POSIX half, so the two could not
    # even run side by side (#1284). A value passed explicitly is honoured and is
    # then PROBED first, since the caller chose the collision risk.
    [int]$Port = 0,
    [string]$Cert = "",
    [string]$Key = ""
)

$ErrorActionPreference = "Stop"
$SKIP = 77

# The skips run FIRST and the port pre-flight second, deliberately: a missing
# binary is a missing prerequisite, and reporting it as a port collision would
# name the wrong subject.
if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Cert) -or -not (Test-Path $Key)) { Write-Host "cert/key not found; skipping"; exit $SKIP }

Import-Module (Join-Path $PSScriptRoot "lib/E2EPorts.psm1") -Force
$Port = Get-E2EFixturePort $Port $Fastcached "fastcached (TLS)"

$log = Join-Path ([System.IO.Path]::GetTempPath()) ('tls-smoke-' + [System.IO.Path]::GetRandomFileName() + '.log')
$proc = Start-Process -FilePath $Fastcached `
    -ArgumentList "--bind=127.0.0.1", "--port=$Port", "--tls", "--tls-cert=$Cert", "--tls-key=$Key" `
    -PassThru -NoNewWindow -RedirectStandardOutput $log -RedirectStandardError "${log}.err"
try {
    # Bounded, and it says what it waited for. The flat `Start-Sleep -Milliseconds
    # 800` this replaces reported a daemon that had ALREADY EXITED as
    # `the target machine actively refused it` — a true observation naming the
    # wrong subject, which is the shape `.agent/rules/testing.md` asks a wait to
    # separate. Measured against a build with no TLS compiled in, where the daemon
    # prints its reason and stops.
    Wait-E2EPortAnswers $Port $proc "fastcached with TLS" @($log, "${log}.err")
    $tcp = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $Port)
    # Self-signed: accept any server certificate for the smoke test.
    $accept = { $true } -as [System.Net.Security.RemoteCertificateValidationCallback]
    $ssl = New-Object System.Net.Security.SslStream($tcp.GetStream(), $false, $accept)
    $ssl.AuthenticateAsClient('localhost')

    $enc = [System.Text.Encoding]::ASCII
    $req = $enc.GetBytes("*1`r`n`$4`r`nPING`r`n")
    $ssl.Write($req, 0, $req.Length); $ssl.Flush()
    Start-Sleep -Milliseconds 200
    $buf = New-Object byte[] 64
    $n = $ssl.Read($buf, 0, $buf.Length)
    $resp = $enc.GetString($buf, 0, $n)
    $ssl.Close(); $tcp.Close()

    Write-Host "response: $resp"
    if ($resp -match 'PONG') { Write-Host "TLS smoke OK"; exit 0 } else { Write-Host "no PONG over TLS"; exit 1 }
}
finally {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Remove-Item -Force $log, "${log}.err" -ErrorAction SilentlyContinue
}
