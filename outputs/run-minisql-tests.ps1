param(
    [ValidateSet('compiler', 'execution', 'storage', 'http', 'frontend', 'all')]
    [string]$Suite = 'compiler',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'
$outputs = $PSScriptRoot
$backend = Join-Path $outputs 'minisql-backend'
$frontend = Join-Path $outputs 'minisql-workbench'

function Invoke-NodeTests([string[]]$Files) {
    Push-Location $backend
    try {
        foreach ($file in $Files) {
            Write-Host "`n>>> node $file" -ForegroundColor Cyan
            node $file
            if ($LASTEXITCODE -ne 0) { throw "Test failed: $file" }
        }
    } finally { Pop-Location }
}

function Invoke-FrontendTests {
    Push-Location $frontend
    try {
        foreach ($script in @('test:safety', 'test:history', 'test:csv', 'build')) {
            Write-Host "`n>>> npm.cmd run $script" -ForegroundColor Cyan
            npm.cmd run $script
            if ($LASTEXITCODE -ne 0) { throw "Frontend step failed: $script" }
        }
    } finally { Pop-Location }
}

if ($Build) {
    Write-Host "`n>>> cmake build Release" -ForegroundColor Cyan
    Push-Location $backend
    try {
        cmake --build build/windows --config Release --parallel 4
        if ($LASTEXITCODE -ne 0) { throw 'Backend build failed.' }
    } finally { Pop-Location }
}

$groups = @{
    compiler = @(
        'tests/parser-regression.mjs',
        'tests/planner-regression.mjs',
        'tests/diagnostics-smoke.mjs',
        'tests/subquery-smoke.mjs',
        'tests/explain-smoke.mjs',
        'tests/statistics-smoke.mjs'
    )
    execution = @(
        'tests/database-process.mjs',
        'tests/update-process.mjs',
        'tests/join-process.mjs',
        'tests/outer-join-smoke.mjs',
        'tests/null-process.mjs',
        'tests/aggregate-process.mjs',
        'tests/avg-process.mjs',
        'tests/decimal-arithmetic-process.mjs',
        'tests/float-process.mjs',
        'tests/transaction-process.mjs'
        'tests/transaction-savepoint.mjs'
    )
    storage = @(
        'tests/checkpoint-smoke.mjs',
        'tests/auto-checkpoint-smoke.mjs',
        'tests/x22-fault-injection.mjs',
        'tests/journal-process.mjs',
        'tests/index-smoke.mjs',
        'tests/backup-smoke.mjs',
        'tests/external-sort-smoke.mjs',
        'tests/external-aggregate-smoke.mjs'
    )
    http = @(
        'tests/database-http.mjs',
        'tests/session-http.mjs',
        'tests/multi-session-http.mjs',
        'tests/access-control-http.mjs',
        'tests/observability-http.mjs',
        'tests/cancel-smoke.mjs',
        'tests/result-budget-smoke.mjs',
        'tests/x25-stream-http.mjs'
    )
}

$selected = if ($Suite -eq 'all') { @('compiler', 'execution', 'storage', 'http', 'frontend') } else { @($Suite) }
foreach ($group in $selected) {
    if ($group -eq 'frontend') { Invoke-FrontendTests; continue }
    Invoke-NodeTests $groups[$group]
}

Write-Host "`nAll selected tests passed ($Suite)." -ForegroundColor Green
