param([string]$InstallRoot)
# 可选参数：依赖安装目录；不传就用项目内的默认位置。
. "$PSScriptRoot/environment.ps1"
# 点源引入环境脚本，拿到 projectRoot 与 vcpkg 的位置。
if (!$InstallRoot) { $InstallRoot = Join-Path $projectRoot '.tools/core-deps' }
# 没指定安装目录时，默认装到项目内的 .tools/core-deps，避免污染系统目录。
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
# 转成绝对路径，防止后面 vcpkg 按相对路径生成到意外位置。
& (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') install "--x-manifest-root=$(Join-Path $projectRoot 'core-deps')" "--x-install-root=$InstallRoot" --triplet=x64-windows
# 用 vcpkg 按清单文件安装核心依赖，目标是 Windows x64 三元组。
# x-manifest-root 指向项目里的清单目录，x-install-root 指定安装位置。
if ($LASTEXITCODE -ne 0) { throw "Core dependency installation failed: $LASTEXITCODE" }
# 安装失败时把退出码带进错误信息，方便排查是哪一步出问题。
Write-Output "Core include directory: $(Join-Path $InstallRoot 'x64-windows/include')"
# 最后打印头文件目录，调用方可以直接拿它去配 CMake 的包含路径。
