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
        foreach ($script in @('test:safety', 'test:history', 'test:csv', 'test:format', 'test:virtual', 'test:demo', 'build')) {
            Write-Host "`n>>> npm.cmd run $script" -ForegroundColor Cyan
            npm.cmd run $script
            if ($LASTEXITCODE -ne 0) { throw "Frontend step failed: $script" }
        }
    } finally { Pop-Location }
}

if (-not (Test-Path -LiteralPath (Join-Path $frontend 'node_modules'))) {
    Write-Host "`n>>> npm.cmd ci" -ForegroundColor Cyan
    Push-Location $frontend
    try {
        npm.cmd ci
        if ($LASTEXITCODE -ne 0) { throw 'Frontend dependency installation failed.' }
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

# 仓库里 43 个进程测试从 bin/ 读取可执行体（bin/ 被 .gitignore 忽略，属于本地产物）。
# 不同步的话，回归会静默地验证上一次构建的旧二进制 —— 曾因此把一个真实的栈溢出
# 崩溃误判成“既有失败”。跑测试前把最新构建产物同步回 bin/。
$releaseDir = Join-Path $backend 'build/windows/Release'
if (Test-Path -LiteralPath $releaseDir) {
    $binDir = Join-Path $backend 'bin'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    $synced = 0
    Get-ChildItem -Path (Join-Path $releaseDir '*.exe'), (Join-Path $releaseDir '*.dll') -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $binDir $_.Name) -Force
        $synced++
    }
    # journal_probe.exe 是 tests/journal-process.mjs 期望的文件名。
    $journalProbe = Join-Path $releaseDir 'minisql_journal_probe.exe'
    if (Test-Path -LiteralPath $journalProbe) {
        Copy-Item -LiteralPath $journalProbe -Destination (Join-Path $binDir 'journal_probe.exe') -Force
        $synced++
    }
    Write-Host "`n>>> synced $synced build artifacts into bin/" -ForegroundColor Cyan
} else {
    Write-Host "`n>>> build/windows/Release not found; using existing bin/ artifacts" -ForegroundColor Yellow
}

$groups = @{
    compiler = @(
        'tests/parser-regression.mjs',
        'tests/planner-regression.mjs',
        'tests/diagnostics-smoke.mjs',
        'tests/subquery-smoke.mjs',
        'tests/explain-smoke.mjs',
        'tests/statistics-smoke.mjs',
        'tests/analyze-stats-smoke.mjs',
        'tests/correlated-aggregate-smoke.mjs',
        'tests/decorrelate-apply-smoke.mjs',
        'tests/sql-dialect-smoke.mjs'
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
        'tests/transaction-overflow.mjs'
        'tests/requirements-limits-process.mjs'
        'tests/nesting-depth-process.mjs'
    )
    storage = @(
        'tests/checkpoint-smoke.mjs',
        'tests/auto-checkpoint-smoke.mjs',
        'tests/x22-fault-injection.mjs',
        'tests/background-checkpoint-fault-injection.mjs',
        'tests/cli-input-and-buffer-log.mjs',
        'tests/journal-process.mjs',
        'tests/wal-metadata-process.mjs',
        'tests/wal-group-commit.mjs',
        'tests/doublewrite-torn-page.mjs',
        'tests/index-smoke.mjs',
        'tests/incremental-index-process.mjs',
        'tests/index-transaction-process.mjs',
        'tests/index-performance-curve.mjs',
        'tests/backup-smoke.mjs',
        'tests/external-sort-smoke.mjs',
        'tests/external-aggregate-smoke.mjs',
        'tests/query-resource-process.mjs'
    )
    http = @(
        'tests/database-http.mjs',
        'tests/session-http.mjs',
        'tests/multi-session-http.mjs',
        'tests/access-control-http.mjs',
        'tests/access-binding-process.mjs',
        'tests/requirements-http-contract.mjs',
        'tests/observability-http.mjs',
        'tests/cancel-smoke.mjs',
        'tests/result-budget-smoke.mjs',
        'tests/x25-stream-http.mjs'
        'tests/session-stream-process.mjs'
    )
}

$selected = if ($Suite -eq 'all') { @('compiler', 'execution', 'storage', 'http', 'frontend') } else { @($Suite) }
foreach ($group in $selected) {
    if ($group -eq 'frontend') { Invoke-FrontendTests; continue }
    Invoke-NodeTests $groups[$group]
}

Write-Host "`nAll selected tests passed ($Suite)." -ForegroundColor Green
