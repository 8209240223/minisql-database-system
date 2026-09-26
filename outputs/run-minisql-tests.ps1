param(
    [ValidateSet('compiler', 'execution', 'storage', 'http', 'fuzz', 'frontend', 'all')]
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
            # generate-corpus.mjs 默认把语料写回 tests/fuzz/，那是受版本控制的目录：
            # 直接跑会把 6 个 corpus-*.sql 与 manifest.json（含 generatedAt 与
            # generatorSha256）改脏，工作区平白多出 7 个"已修改"文件。
            # 它认 FUZZ_CORPUS_DIR，指向临时目录即可，产物仍逐字节校验。
            $generator = $file -eq 'tests/fuzz/generate-corpus.mjs'
            $previous = $env:FUZZ_CORPUS_DIR
            if ($generator) { $env:FUZZ_CORPUS_DIR = Join-Path ([System.IO.Path]::GetTempPath()) ("minisql-corpus-" + [guid]::NewGuid().ToString('N')) }
            try { node $file } finally {
                if ($generator) { $env:FUZZ_CORPUS_DIR = $previous }
            }
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
    # 测试引用的是构建产物原名（minisql_journal_probe.exe 等），这里不做任何别名映射，
    # 保证 bin/ 是构建输出的忠实镜像，避免出现“测试找不到可执行体”的静默 ENOENT。
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
        # 索引建议器：基于查询负载给出建索引建议（含建议可执行、建完即消失的闭环）。
        'tests/index-advisor-smoke.mjs'
        # 派生表、IN 列表与相关子查询执行；聚合的语法/AST/计划三段契约。
        'tests/derived-smoke.mjs'
        'tests/correlated-exec-smoke.mjs'
        'tests/in-list-smoke.mjs'
        'tests/aggregate-parser.mjs'
        'tests/aggregate-plan.mjs'
        'tests/aggregate-execution-gate.mjs'
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
        # SQL 扩展功能：LIKE 模式匹配、CROSS JOIN 与逗号连接。
        'tests/like-smoke.mjs'
        'tests/cross-join-smoke.mjs'
        # 目录持久化与类型编解码往返。
        'tests/catalog-persistence-smoke.mjs'
        'tests/type-roundtrip-smoke.mjs'
        # 查询结果缓存：命中正确性 + 写入后失效。
        'tests/query-result-cache-smoke.mjs'
        # Top-N 排序下推：Limit 下推到 Sort，只保留前 N 行（含并列键稳定性）。
        'tests/top-n-sort-smoke.mjs'
        # 有序索引扫描：排序键为非空单列索引时用索引顺序替代排序。
        'tests/index-order-scan-smoke.mjs'
        # 类型列、别名、DEFAULT 与 INSERT 形态（列映射/表达式/多行）。
        'tests/alias-process.mjs'
        'tests/bigint-process.mjs'
        'tests/bool-column-process.mjs'
        'tests/cast-process.mjs'
        'tests/date-column-process.mjs'
        'tests/date-literal-process.mjs'
        'tests/decimal-cast-process.mjs'
        'tests/decimal-column-process.mjs'
        'tests/decimal-literal-process.mjs'
        'tests/default-process.mjs'
        'tests/varchar-column-process.mjs'
        'tests/insert-columns-process.mjs'
        'tests/insert-expression-process.mjs'
        'tests/multirow-process.mjs'
        # 约束：CHECK、命名约束、单列/复合键、外键与自引用外键。
        'tests/check-process.mjs'
        'tests/named-constraint-process.mjs'
        'tests/composite-key-process.mjs'
        'tests/foreign-key-process.mjs'
        'tests/composite-foreign-key-process.mjs'
        'tests/self-foreign-key-process.mjs'
        'tests/unique-process.mjs'
    )
    storage = @(
        'tests/checkpoint-smoke.mjs',
        'tests/auto-checkpoint-smoke.mjs',
        'tests/x22-fault-injection.mjs',
        'tests/background-checkpoint-fault-injection.mjs',
        'tests/cli-input-and-buffer-log.mjs'
        # 缓存替换策略（LRU / FIFO / CLOCK）与顺序扫描的页局部性。
        'tests/cache-policy-smoke.mjs',
        'tests/journal-process.mjs',
        'tests/wal-metadata-process.mjs',
        'tests/wal-group-commit.mjs',
        'tests/doublewrite-torn-page.mjs',
        'tests/index-smoke.mjs',
        'tests/incremental-index-process.mjs',
        'tests/index-transaction-process.mjs',
        'tests/index-performance-curve.mjs',
        'tests/backup-smoke.mjs',
        'tests/backup-online-smoke.mjs',
        'tests/external-sort-smoke.mjs',
        'tests/external-aggregate-smoke.mjs',
        'tests/query-resource-process.mjs'
        # 写批次契约、probe 级差分与索引规模回归（默认 1000 行）。
        'tests/write-batch-process.mjs'
        'tests/arithmetic64-differential.mjs'
        'tests/decimal-journal-process.mjs'
        'tests/index-scale-smoke.mjs'
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
        # 会话协议与传输：持久会话、分片响应、队列与畸形帧。
        'tests/session-process.mjs'
        'tests/session-transport.mjs'
        # 遗留 bridge 转发与权限感知 CLI。
        'tests/bridge-regression.mjs'
        'tests/cli-contract.mjs'
        # 权限目录：纯 Node 契约 + 引擎入口闭环。
        'tests/access-store-contract.mjs'
        'tests/access-catalog-atomic-contract.mjs'
        'tests/access-atomic-http.mjs'
        'tests/access-control-process.mjs'
        # 行流资源回传契约（会话级多帧流）。
        'tests/x25-row-stream-contract.mjs'
    )
    fuzz = @(
        # X27：固定种子语料的生成、逐字节复现与状态机差分/长跑/压力/崩溃恢复。
        'tests/fuzz-model-contract.mjs'
        'tests/fuzz-process-contract.mjs'
        'tests/fuzz-differential.mjs'
        'tests/fuzz-state-machine-differential.mjs'
        'tests/fuzz-state-machine-crash-recovery.mjs'
        'tests/fuzz-state-machine-long-run.mjs'
        'tests/fuzz-state-machine-soak.mjs'
        'tests/fuzz-state-machine-pressure.mjs'
        'tests/fuzz-state-machine-replay-contract.mjs'
        'tests/fuzz/generate-corpus.mjs'
        'tests/fuzz/reproducibility-contract.mjs'
    )
}

$selected = if ($Suite -eq 'all') { @('compiler', 'execution', 'storage', 'http', 'fuzz', 'frontend') } else { @($Suite) }
foreach ($group in $selected) {
    if ($group -eq 'frontend') { Invoke-FrontendTests; continue }
    Invoke-NodeTests $groups[$group]
}

Write-Host "`nAll selected tests passed ($Suite)." -ForegroundColor Green
