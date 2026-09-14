# Extract a verified official build tool into .tools; no system installation.
# 把一份经过校验的官方构建工具解压到项目内的 .tools，不做系统级安装。
$ErrorActionPreference = 'Stop'
# 任何命令失败都直接终止，避免带着坏状态继续。
$projectRoot = Split-Path -Parent $PSScriptRoot
# 项目根目录 = 本脚本所在目录的上级。
$toolDirectory = Join-Path $projectRoot '.tools/pkgconf'
# pkgconf 的安装目标目录。
$toolExecutable = Join-Path $toolDirectory 'PFiles64/pkgconf-3.0.7/pkgconf.exe'
# 解压后可执行文件的固定位置（msiexec 的 /a 会保留这个子目录结构）。
if (Test-Path -LiteralPath $toolExecutable) {
# 已经装过就直接返回。
    Write-Host "pkgconf ready: $toolExecutable"
    # 打印路径，调用方可以据此配 PATH。
    exit 0
    # 用 0 退出码表示"无需操作且成功"。
}
# 存在性检查结束。
$downloadDirectory = Join-Path $projectRoot '.tools/downloads'
# 下载缓存目录（与 vcpkg 共用同一处，便于统一清理）。
New-Item -ItemType Directory -Force -Path $downloadDirectory | Out-Null
# 先建目录。
$installer = Join-Path $downloadDirectory 'pkgconf-x64-3.0.7.msi'
# 安装包在本地的落点。
if (!(Test-Path -LiteralPath $installer)) {
# 本地还没有才下载，避免每次都重新拉一遍。
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/pkgconf/pkgconf/releases/download/pkgconf-3.0.7/pkgconf-x64-3.0.7.msi' -OutFile $installer
    # 从官方 release 下载指定版本的安装包。
}
# 下载分支结束。
$expectedHash = '7a316dba4a4498ea952b746c82deed41c597657095a0f776b75654191b45ae44'
# 预先记录的 SHA-256，用来确认下载到的确实是官方那份。
if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $expectedHash) {
# 现场算一次哈希并比对。
    throw 'pkgconf checksum mismatch. Remove the failed download and retry.'
    # 不一致说明文件损坏或被替换，直接报错并提示怎么恢复。
}
# 校验结束。
$installerProcess = Start-Process msiexec.exe -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
# 用 msiexec 的"管理式安装"（/a）把 MSI 解包到指定目录，而不是真的装进系统。
    '/a', ('"' + $installer + '"'), '/qn', ('TARGETDIR="' + $toolDirectory + '"'))
    # /a 表示解包，/qn 表示无界面，TARGETDIR 指定解包目标。
if ($installerProcess.ExitCode -ne 0 -or !(Test-Path -LiteralPath $toolExecutable)) {
# 退出码非 0，或者解包后目标文件仍不存在，都算失败。
    throw "pkgconf extraction failed with code $($installerProcess.ExitCode)."
    # 错误信息里带上退出码，便于定位。
}
# 失败检查结束。
Write-Host "pkgconf ready: $toolExecutable"
# 成功时打印可执行文件路径。
