$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$backend = Join-Path $root 'minisql-backend'
$frontend = Join-Path $root 'minisql-workbench'
if (-not (Test-Path (Join-Path $backend 'scripts/database-bridge.mjs'))) { throw 'Backend bridge not found.' }
if (-not (Test-Path (Join-Path $frontend 'package.json'))) { throw 'Frontend workspace not found.' }
if (-not (Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath 'node' -ArgumentList 'scripts/database-bridge.mjs' -WorkingDirectory $backend -WindowStyle Hidden
}
$demoDatabase = Join-Path $backend 'data/demo.pages'
$demoAccessFile = Join-Path $backend 'data/demo-access.catalog.pages'
if (-not (Get-NetTCPConnection -LocalPort 8082 -State Listen -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath 'node' -ArgumentList 'scripts/database-bridge.mjs','--port','8082','--database',$demoDatabase,'--access-file',$demoAccessFile -WorkingDirectory $backend -WindowStyle Hidden
}
if (-not (Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)) {
  Start-Process -FilePath 'npm.cmd' -ArgumentList 'run','dev','--','--host','127.0.0.1','--port','4173' -WorkingDirectory $frontend -WindowStyle Hidden
}
for ($i = 0; $i -lt 30; $i++) {
  $apiReady = [bool](Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)
  $demoReady = [bool](Get-NetTCPConnection -LocalPort 8082 -State Listen -ErrorAction SilentlyContinue)
  $webReady = [bool](Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)
  if ($apiReady -and $demoReady -and $webReady) { break }
  Start-Sleep -Milliseconds 500
}
if (-not (Get-NetTCPConnection -LocalPort 8081 -State Listen -ErrorAction SilentlyContinue)) { throw 'C++ bridge startup timed out.' }
if (-not (Get-NetTCPConnection -LocalPort 8082 -State Listen -ErrorAction SilentlyContinue)) { throw 'Demo bridge startup timed out.' }
if (-not (Get-NetTCPConnection -LocalPort 4173 -State Listen -ErrorAction SilentlyContinue)) { throw 'Frontend startup timed out.' }
$health = Invoke-RestMethod 'http://127.0.0.1:8081/api/health'
if ($health.status -ne 'ok') { throw "C++ bridge health check failed: $($health.status)" }
$demoHealth = Invoke-RestMethod 'http://127.0.0.1:8082/api/health'
if ($demoHealth.status -ne 'ok') { throw "Demo bridge health check failed: $($demoHealth.status)" }
$previousDemoApi = $env:MINISQL_API
try {
  $env:MINISQL_API = 'http://127.0.0.1:8082/api'
  Push-Location $backend
  try { node scripts/seed-workbench.mjs } finally { Pop-Location }
} finally {
  if ($null -eq $previousDemoApi) { Remove-Item Env:MINISQL_API -ErrorAction SilentlyContinue }
  else { $env:MINISQL_API = $previousDemoApi }
}
Write-Output 'MiniSQL bridge: http://127.0.0.1:8081/api'
Write-Output 'MiniSQL demo bridge: http://127.0.0.1:8082/api'
Write-Output 'MiniSQL workbench: http://127.0.0.1:4173'
