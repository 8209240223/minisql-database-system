param(
    [switch]$SkipBrowser,
    [switch]$SkipExtended
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$backend = Join-Path $root 'outputs/minisql-backend'
$frontend = Join-Path $root 'outputs/minisql-workbench'
$verificationBuild = Join-Path $backend 'build/verification'
$servicesStarted = $false

function Invoke-Checked([string]$Name, [scriptblock]$Action) {
    Write-Host "`n>>> $Name" -ForegroundColor Cyan
    & $Action
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE." }
}

if (-not (Test-Path -LiteralPath (Join-Path $backend 'CMakeLists.txt'))) {
    throw 'Run verify-clean.ps1 from the MiniSQL repository root.'
}

. (Join-Path $backend 'scripts/environment.ps1')

# This directory is reserved for verification, so removing it cannot affect a
# developer's normal Debug/Release build trees.
if (Test-Path -LiteralPath $verificationBuild) {
    Remove-Item -LiteralPath $verificationBuild -Recurse -Force
}

Push-Location $backend
try {
    Invoke-Checked 'Configure clean Release build' {
        & $cmakeExe -S . -B $verificationBuild -G 'Visual Studio 17 2022' -A x64 `
            -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
            -DVCPKG_TARGET_TRIPLET=x64-windows `
            -DVCPKG_OVERLAY_TRIPLETS="$backend/cmake/triplets" `
            -DBUILD_TESTING=ON
    }
    Invoke-Checked 'Build all Release targets' {
        & $cmakeExe --build $verificationBuild --config Release --parallel
    }
    Invoke-Checked 'Run registered C++ tests' {
        & $ctestExe --test-dir $verificationBuild -C Release --output-on-failure --no-tests=error
    }

    $env:MINISQL_EXE = Join-Path $verificationBuild 'Release/minisql_database.exe'
    if (-not $SkipExtended) {
        foreach ($suite in @('compiler', 'execution', 'storage', 'http')) {
            Invoke-Checked "Run $suite regressions" {
                & (Join-Path $root 'outputs/run-minisql-tests.ps1') -Suite $suite
            }
        }
    }
} finally {
    Pop-Location
}

Push-Location $frontend
try {
    Invoke-Checked 'Install exact frontend dependency graph' { npm.cmd ci }
    Invoke-Checked 'Build workbench' { npm.cmd run build }
    foreach ($script in @('test:safety', 'test:history', 'test:csv', 'test:format', 'test:virtual')) {
        Invoke-Checked "Run workbench $script" { npm.cmd run $script }
    }
    if (-not $SkipBrowser) {
        try {
            Invoke-Checked 'Start isolated workbench services' {
                & (Join-Path $root 'outputs/start-minisql-workbench.ps1')
            }
            $servicesStarted = $true
            Invoke-Checked 'Run workbench browser regressions' { npm.cmd run test:browser }
        } finally {
            if ($servicesStarted) {
                & (Join-Path $root 'outputs/stop-minisql-workbench.ps1')
            }
        }
    }
} finally {
    Pop-Location
}

Write-Host "`nClean verification passed." -ForegroundColor Green
