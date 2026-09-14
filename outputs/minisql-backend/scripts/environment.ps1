$ErrorActionPreference = 'Stop'
# 全局策略：任何命令失败都当作致命错误，避免脚本"带病继续"。
$projectRoot = Split-Path -Parent $PSScriptRoot
# 项目根目录 = 本脚本所在目录的上级，所有相对路径都以它为基准。
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
# 先看系统 PATH 里有没有 cmake；找不到不报错，留给下面走 Visual Studio 内置的那份。
$vsInstallation = $null
# 保存 Visual Studio 的安装路径，初始为空。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
# vswhere 是微软官方的 VS 探测工具，装在固定的 Program Files (x86) 位置。
if (Test-Path -LiteralPath $vswhere) {
# 只有探测工具存在时才尝试。
    $vsInstallation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    # 取"最新的、带 C++ 编译工具组件"的那个 VS 实例的安装路径。
}
# VS 探测结束。
if ($cmakeCommand) {
# 优先使用 PATH 里的 cmake。
    $cmakeExe = $cmakeCommand.Source
    # 记下它的绝对路径。
} elseif ($vsInstallation) {
# 否则退回 Visual Studio 自带的那份 CMake。
    $cmakeExe = Join-Path $vsInstallation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
    # 这是 VS 内置 CMake 的固定相对位置。
} else {
# 两条路都走不通。
    throw 'CMake not found. Install CMake or Visual Studio C++ tools.'
    # 明确报错并给出解决方向，而不是让后续命令莫名失败。
}
# CMake 定位结束。
if (!(Test-Path -LiteralPath $cmakeExe)) { throw "CMake not found: $cmakeExe" }
# 再确认一次这个路径确实存在，防止拿到一个失效的候选项。
$ctestExe = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
# CTest 与 CMake 同目录，直接由 CMake 的路径推出。
$env:PATH = "$(Split-Path -Parent $cmakeExe);$env:PATH"
# 把这个目录临时加到进程 PATH 前面，保证后续调用拿到的是同一份 CMake。
if (!$env:VCPKG_ROOT -and $vsInstallation) {
# 用户没设 VCPKG_ROOT 但装了 VS 时，先猜 VS 自带的 vcpkg。
    $env:VCPKG_ROOT = Join-Path $vsInstallation 'VC/vcpkg'
    # 记下猜测结果，下面还会校验。
}
# vcpkg 猜测结束。
if (!$env:VCPKG_ROOT -or !(Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'))) {
# 用"关键文件是否存在"来验证 VCPKG_ROOT 真的指向一份可用的 vcpkg。
    throw 'Set VCPKG_ROOT to an existing vcpkg installation.'
    # 不满足就报错，避免构建到一半才发现依赖工具链缺失。
}
# vcpkg 校验结束。
# Keep caches local to this project; no machine-wide installation or PATH change.
# 缓存一律留在项目内，不改系统环境、不动全局 PATH。
if (!$env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $projectRoot '.tools/downloads' }
# 下载缓存目录（没设才设，尊重用户已有配置）。
if (!$env:VCPKG_REGISTRIES_CACHE) { $env:VCPKG_REGISTRIES_CACHE = Join-Path $projectRoot '.tools/registries' }
# 注册表缓存目录，同样只在未设置时兜底。
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS, $env:VCPKG_REGISTRIES_CACHE | Out-Null
# 先把这两个目录建出来，否则 vcpkg 首次运行会失败。
if (!$env:VCPKG_BINARY_SOURCES) {
# 没配置二进制缓存源时，
    $binaryCache = Join-Path $projectRoot '.tools/binary-cache'
    # 用项目内的二进制缓存目录。
    New-Item -ItemType Directory -Force -Path $binaryCache | Out-Null
    # 先建目录。
    $env:VCPKG_BINARY_SOURCES = "clear;files,$binaryCache,readwrite"
    # 清掉默认源，改成"文件缓存 + 可读可写"，这样重复构建能直接复用已编译的依赖。
}
# 二进制缓存配置结束。
$env:VCPKG_DISABLE_METRICS = '1'
# 关闭 vcpkg 的遥测上报。
# Reuse the detected CMake instead of downloading another large tool bundle.
# 复用上面探到的 CMake，而不是让 vcpkg 再下载一整套工具。
# Do not force system binaries: vcpkg may download a pinned PowerShell (pwsh)
# 不要强制使用系统二进制：vcpkg 可能需要下载一份固定版本的 pwsh
# for its toolchain bootstrap through the configured proxy when the host lacks
# （在主机缺少 pwsh 时，它会通过配置的代理去下载），
# pwsh, which the old FORCE flag silently prevented and broke "vcpkg install".
# 而旧写法里的 FORCE 标志会悄悄阻止这次下载，最终让 vcpkg install 失败。
# Git and CMake still come from the system PATH.
# Git 与 CMake 仍然来自系统 PATH。
