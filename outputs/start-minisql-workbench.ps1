$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$backend = Join-Path $root 'minisql-backend'
$frontend = Join-Path $root 'minisql-workbench'
if (-not (Test-Path (Join-Path $backend 'scripts/database-bridge.mjs'))) { throw 'Backend bridge not found.' }
if (-not (Test-Path (Join-Path $frontend 'package.json'))) { throw 'Frontend workspace not found.' }
if (-not (Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath 'node' -ArgumentList 'scripts/database-bridge.mjs' -WorkingDirectory $backend -WindowStyle Hidden
}
if (-not (Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath 'npm.cmd' -ArgumentList 'run','dev','--','--host','127.0.0.1','--port','4173' -WorkingDirectory $frontend -WindowStyle Hidden
}
for ($i = 0; $i -lt 30; $i++) {
  $apiReady = [bool](Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)
  $webReady = [bool](Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)
  if ($apiReady -and $webReady) { break }
  Start-Sleep -Milliseconds 500
}
if (-not (Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)) { throw 'C++ bridge startup timed out.' }
if (-not (Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)) { throw 'Frontend startup timed out.' }
$health = Invoke-RestMethod 'http://127.0.0.1:8081/api/health'
if ($health.status -ne 'ok') { throw "C++ bridge health check failed: $($health.status)" }
Write-Output 'MiniSQL bridge: http://127.0.0.1:8081/api'
Write-Output 'MiniSQL workbench: http://127.0.0.1:4173'
