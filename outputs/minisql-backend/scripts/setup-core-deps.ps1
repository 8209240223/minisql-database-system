param([string]$InstallRoot)
. "$PSScriptRoot/environment.ps1"
if (!$InstallRoot) { $InstallRoot = Join-Path $projectRoot '.tools/core-deps' }
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
& (Join-Path $env:VCPKG_ROOT 'vcpkg.exe') install "--x-manifest-root=$(Join-Path $projectRoot 'core-deps')" "--x-install-root=$InstallRoot" --triplet=x64-windows
if ($LASTEXITCODE -ne 0) { throw "Core dependency installation failed: $LASTEXITCODE" }
Write-Output "Core include directory: $(Join-Path $InstallRoot 'x64-windows/include')"
