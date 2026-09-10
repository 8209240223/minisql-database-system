$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$vsInstallation = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $vsInstallation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($cmakeCommand) {
    $cmakeExe = $cmakeCommand.Source
} elseif ($vsInstallation) {
    $cmakeExe = Join-Path $vsInstallation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
} else {
    throw 'CMake not found. Install CMake or Visual Studio C++ tools.'
}
if (!(Test-Path -LiteralPath $cmakeExe)) { throw "CMake not found: $cmakeExe" }
$ctestExe = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
$env:PATH = "$(Split-Path -Parent $cmakeExe);$env:PATH"
if (!$env:VCPKG_ROOT -and $vsInstallation) {
    $env:VCPKG_ROOT = Join-Path $vsInstallation 'VC/vcpkg'
}
if (!$env:VCPKG_ROOT -or !(Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake'))) {
    throw 'Set VCPKG_ROOT to an existing vcpkg installation.'
}
# Keep caches local to this project; no machine-wide installation or PATH change.
if (!$env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS = Join-Path $projectRoot '.tools/downloads' }
if (!$env:VCPKG_REGISTRIES_CACHE) { $env:VCPKG_REGISTRIES_CACHE = Join-Path $projectRoot '.tools/registries' }
New-Item -ItemType Directory -Force -Path $env:VCPKG_DOWNLOADS, $env:VCPKG_REGISTRIES_CACHE | Out-Null
if (!$env:VCPKG_BINARY_SOURCES) {
    $binaryCache = Join-Path $projectRoot '.tools/binary-cache'
    New-Item -ItemType Directory -Force -Path $binaryCache | Out-Null
    $env:VCPKG_BINARY_SOURCES = "clear;files,$binaryCache,readwrite"
}
$env:VCPKG_DISABLE_METRICS = '1'
# 不设置 VCPKG_FORCE_SYSTEM_BINARIES，让 vcpkg 按需下载 cmake/powershell-core 等工具；
# 该变量一旦非空会导致 powershell-core 等工具无法下载（本仓库的 VS 内置 vcpkg 无内置
# ports 目录，依赖 vcpkg-configuration.json 的 git 注册表解析 baseline）。
