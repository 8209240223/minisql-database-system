@echo off
chcp 65001 >nul
setlocal
title MiniSQL Studio
set "MINISQL_START_SCRIPT=%~dp0outputs\start-minisql-workbench.ps1"
if not exist "%MINISQL_START_SCRIPT%" (
    echo 找不到项目启动脚本，请将此文件放在 MiniSQL 项目根目录。
    pause
    exit /b 1
)
echo 正在启动 MiniSQL，请稍候...
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; try { & $env:MINISQL_START_SCRIPT; $response = Invoke-WebRequest 'http://127.0.0.1:4173' -UseBasicParsing -TimeoutSec 20; if ($response.StatusCode -ne 200) { throw 'Frontend health check failed.' }; Start-Process 'http://127.0.0.1:4173'; exit 0 } catch { Write-Host $_.Exception.Message -ForegroundColor Red; exit 1 }"
if errorlevel 1 (
    echo 启动失败，请查看上方错误信息。
    pause
    exit /b 1
)
exit /b 0
