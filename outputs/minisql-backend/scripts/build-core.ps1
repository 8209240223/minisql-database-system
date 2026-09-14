param(
# 脚本参数块。
    [Parameter(Mandatory = $true)][string]$JsonInclude,
    # 必填：包含 nlohmann/json.hpp 与 Boost 头文件的目录（由 setup-core-deps.ps1 产出）。
    [ValidateSet('compile','database','database-test','optimizer-test','heap-test','arithmetic64-test','batch-test','storage-test','buffer-test','journal-test','aggregate-semantic-test','decimal-test')][string]$Target = 'database',
    # 构建目标：可以是两个可执行程序（compile/database），也可以是各类契约测试。
    # ValidateSet 把可选值写死在参数上，拼错会立刻报错。
    [string]$Compiler = 'g++'
    # 编译器命令，默认 g++；允许换成别的（例如 MSYS2 下的同族编译器）。
)
# 参数块结束。
$ErrorActionPreference = 'Stop'
# 任何失败都终止脚本。
$root = Split-Path -Parent $PSScriptRoot
# 项目根目录。
$includePath = (Resolve-Path -LiteralPath $JsonInclude).Path
# 把传进来的包含目录解析成绝对路径，顺便验证它确实存在。
if (-not (Test-Path -LiteralPath (Join-Path $includePath 'nlohmann/json.hpp'))) {
# 这个目录里必须有 JSON 库的主头文件。
    throw 'JsonInclude must contain nlohmann/json.hpp'
    # 缺了就明确报错，避免编译器报到一堆看不懂的包含错误。
}
# JSON 头检查结束。
if ($Target -ne 'arithmetic64-test' -and
    # 除了 arithmetic64-test（它只用到标准库大整数）之外，
    -not (Test-Path -LiteralPath (Join-Path $includePath 'boost/multiprecision/cpp_int.hpp'))) {
    # 其它目标都需要 Boost.Multiprecision 的头文件。
    throw 'JsonInclude must also contain Boost.Multiprecision; run scripts/setup-core-deps.ps1 first.'
    # 报错时直接告诉用户该先跑哪个脚本。
}
# Boost 头检查结束。
$sources = @('src/common/error.cpp','src/sql/lexer.cpp','src/sql/parser.cpp',
# 基础源文件集合：错误处理与整个 SQL 前端（词法、语法、计划）。
    'src/sql/planner.cpp','src/catalog/catalog.cpp','src/optimizer/optimizer.cpp')
    # 再加上目录层与优化器。
if ($Target -ne 'compile') {
# 除了"只编译"这个目标，其它目标都要带上存储与执行层。
    $sources += @('src/storage/page.cpp','src/storage/file_io.cpp','src/storage/page_file.cpp','src/storage/buffer_pool.cpp',
        # 页、文件 IO、页文件、缓冲池，
        'src/storage/heap.cpp','src/catalog/persistent_catalog.cpp','src/execution/database.cpp')
        # 堆存储、持久化目录与数据库门面。
}
# 追加结束。
if ($Target -eq 'arithmetic64-test') { $sources = @('src/common/error.cpp') }
# 大整数探针只需要错误处理模块，其余一律不要，保持编译最快。
if ($Target -eq 'decimal-test') { $sources = @('src/common/error.cpp') }
# 定点数测试同理。
if ($Target -eq 'aggregate-semantic-test') { $sources = @('src/common/error.cpp','src/sql/lexer.cpp','src/sql/parser.cpp','src/catalog/catalog.cpp') }
# 聚合语义测试需要词法、语法与目录（做语义校验），但不需要存储层。
if ($Target -in @('batch-test','storage-test','buffer-test','journal-test')) { $sources = @('src/common/error.cpp','src/storage/page.cpp','src/storage/file_io.cpp','src/storage/page_file.cpp','src/storage/buffer_pool.cpp','src/storage/heap.cpp') }
# 这四个都只测存储层，所以只编错误处理加存储相关源文件。
$entry = switch ($Target) {
# 按目标选入口文件：可执行程序选 src/server 下的 main，测试选 tests 下的契约测试。
    'compile' { 'src/server/compile_main.cpp' }
    # 只跑词法到优化的编译前端。
    'database' { 'src/server/database_main.cpp' }
    # 完整数据库进程（含会话协议）。
    'database-test' { 'tests/database_contract.cpp' }
    # 数据库契约测试。
    'optimizer-test' { 'tests/optimizer_contract.cpp' }
    # 优化器契约测试。
    'heap-test' { 'tests/heap_catalog_contract.cpp' }
    # 堆存储与目录的联调测试。
    'arithmetic64-test' { 'tests/arithmetic64_probe.cpp' }
    # 64 位整数运算探针。
    'batch-test' { 'tests/write_batch_contract.cpp' }
    # 写批次契约测试。
    'storage-test' { 'tests/storage_contract.cpp' }
    # 存储层契约测试。
    'buffer-test' { 'tests/buffer_contract.cpp' }
    # 缓冲池契约测试。
    'journal-test' { 'tests/journal_probe.cpp' }
    # 日志（WAL）探针。
    'aggregate-semantic-test' { 'tests/aggregate_semantic_contract.cpp' }
    # 聚合语义校验测试。
    'decimal-test' { 'tests/decimal_contract.cpp' }
    # 定点数契约测试。
}
# 入口选择结束。
$outputName = switch ($Target) {
# 生成的可执行文件名，与入口一一对应。
    'compile' { 'minisql_compile.exe' }
    # 编译前端产物。
    'database' { 'minisql_database.exe' }
    # 数据库进程产物。
    'database-test' { 'database_contract.exe' }
    # 各测试产物的名字。
    'optimizer-test' { 'optimizer_contract.exe' }
    # 优化器测试。
    'heap-test' { 'heap_catalog_contract.exe' }
    # 堆与目录测试。
    'arithmetic64-test' { 'arithmetic64_probe.exe' }
    # 大整数探针。
    'batch-test' { 'write_batch_contract.exe' }
    # 写批次测试。
    'storage-test' { 'storage_contract.exe' }
    # 存储测试。
    'buffer-test' { 'buffer_contract.exe' }
    # 缓冲池测试。
    'journal-test' { 'journal_probe.exe' }
    # 日志探针。
    'aggregate-semantic-test' { 'aggregate_semantic_contract.exe' }
    # 聚合语义测试。
    'decimal-test' { 'decimal_contract.exe' }
    # 定点数测试。
}
# 文件名选择结束。
Push-Location $root
# 切到项目根目录，让下面的相对路径（include、src/...）都成立。
try {
# try/finally 保证退出时恢复目录。
    New-Item -ItemType Directory -Force -Path 'bin' | Out-Null
    # 建 bin 目录。
    $arguments = @('-std=c++20','-O0','-Wall','-Wextra','-Werror','-static','-I','include','-I',$includePath)
    # 编译选项：C++20、不优化（便于调试）、打开告警并"把告警当错误"、静态链接，
    # 以及两个包含目录（项目内 include 与外部依赖目录）。
    $arguments += $sources
    # 追加本次目标需要的源文件。
    $arguments += $entry
    # 追加入口文件。
    if ($Target -eq 'database') { $arguments += '-lshell32' }
    # 数据库进程用到 Windows 的 shell 接口（宽字符命令行处理），需要链接 shell32。
    $arguments += @('-o', (Join-Path 'bin' $outputName))
    # 指定输出路径。
    & $Compiler @arguments
    # 用数组展开方式调用编译器：PowerShell 会逐项传参，不用手工拼引号。
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed: $LASTEXITCODE" }
    # 编译失败就把退出码带进错误信息。
    Write-Output "Built bin/$outputName"
    # 成功时打印产物路径。
} finally {
# 收尾块。
    Pop-Location
    # 恢复原来的工作目录。
}
# 脚本结束。
