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
# Exit codes: 0 = +PONG over TLS; 1 = ran but no PONG; 77 = a runtime
# prerequisite (fastcached / cert) was missing — treated as a skip.
param(
    [string]$Fastcached = "",
    [int]$Port = 11811,
    [string]$Cert = "",
    [string]$Key = ""
)

$ErrorActionPreference = "Stop"
$SKIP = 77

if (-not (Test-Path $Fastcached)) { Write-Host "fastcached not found: $Fastcached; skipping"; exit $SKIP }
if (-not (Test-Path $Cert) -or -not (Test-Path $Key)) { Write-Host "cert/key not found; skipping"; exit $SKIP }

$proc = Start-Process -FilePath $Fastcached `
    -ArgumentList "--bind=127.0.0.1", "--port=$Port", "--tls", "--tls-cert=$Cert", "--tls-key=$Key" `
    -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Milliseconds 800
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
}
