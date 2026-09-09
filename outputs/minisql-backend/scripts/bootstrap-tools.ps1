# Extract a verified official build tool into .tools; no system installation.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$toolDirectory = Join-Path $projectRoot '.tools/pkgconf'
$toolExecutable = Join-Path $toolDirectory 'PFiles64/pkgconf-3.0.7/pkgconf.exe'
if (Test-Path -LiteralPath $toolExecutable) {
    Write-Host "pkgconf ready: $toolExecutable"
    exit 0
}
$downloadDirectory = Join-Path $projectRoot '.tools/downloads'
New-Item -ItemType Directory -Force -Path $downloadDirectory | Out-Null
$installer = Join-Path $downloadDirectory 'pkgconf-x64-3.0.7.msi'
if (!(Test-Path -LiteralPath $installer)) {
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/pkgconf/pkgconf/releases/download/pkgconf-3.0.7/pkgconf-x64-3.0.7.msi' -OutFile $installer
}
$expectedHash = '7a316dba4a4498ea952b746c82deed41c597657095a0f776b75654191b45ae44'
if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'pkgconf checksum mismatch. Remove the failed download and retry.'
}
$installerProcess = Start-Process msiexec.exe -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
    '/a', ('"' + $installer + '"'), '/qn', ('TARGETDIR="' + $toolDirectory + '"'))
if ($installerProcess.ExitCode -ne 0 -or !(Test-Path -LiteralPath $toolExecutable)) {
    throw "pkgconf extraction failed with code $($installerProcess.ExitCode)."
}
Write-Host "pkgconf ready: $toolExecutable"
