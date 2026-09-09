param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
. "$PSScriptRoot/environment.ps1"
Push-Location $projectRoot
try {
    & $cmakeExe --preset windows
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & $cmakeExe --build --preset "windows-$($Configuration.ToLowerInvariant())" --parallel
    if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }
} finally { Pop-Location }
