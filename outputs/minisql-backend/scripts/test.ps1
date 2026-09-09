param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug')
& "$PSScriptRoot/build.ps1" -Configuration $Configuration
. "$PSScriptRoot/environment.ps1"
Push-Location $projectRoot
try {
    & $ctestExe --preset "windows-$($Configuration.ToLowerInvariant())"
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
} finally { Pop-Location }
