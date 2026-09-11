$ErrorActionPreference = 'SilentlyContinue'
foreach ($port in 8081, 8082, 4173) {
  $listeners = Get-NetTCPConnection -LocalPort $port -State Listen
  foreach ($listener in $listeners) {
    $process = Get-Process -Id $listener.OwningProcess
    if ($process.ProcessName -in @('node','npm','npm.cmd')) {
      Stop-Process -Id $process.Id -Force
      Write-Output "已停止端口 $port 的 Node 服务（PID $($process.Id)）。"
    }
  }
}
