param(
    [Parameter(Mandatory = $true)][string]$JsonInclude,
    [ValidateSet('compile','database','database-test','optimizer-test','heap-test','arithmetic64-test','batch-test','storage-test','buffer-test','journal-test','aggregate-semantic-test','decimal-test')][string]$Target = 'database',
    [string]$Compiler = 'g++'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$includePath = (Resolve-Path -LiteralPath $JsonInclude).Path
if (-not (Test-Path -LiteralPath (Join-Path $includePath 'nlohmann/json.hpp'))) {
    throw 'JsonInclude must contain nlohmann/json.hpp'
}
if ($Target -ne 'arithmetic64-test' -and
    -not (Test-Path -LiteralPath (Join-Path $includePath 'boost/multiprecision/cpp_int.hpp'))) {
    throw 'JsonInclude must also contain Boost.Multiprecision; run scripts/setup-core-deps.ps1 first.'
}
$sources = @('src/common/error.cpp','src/sql/lexer.cpp','src/sql/parser.cpp',
    'src/sql/planner.cpp','src/catalog/catalog.cpp','src/optimizer/optimizer.cpp')
if ($Target -ne 'compile') {
    $sources += @('src/storage/page.cpp','src/storage/file_io.cpp','src/storage/page_file.cpp','src/storage/buffer_pool.cpp',
        'src/storage/heap.cpp','src/catalog/persistent_catalog.cpp','src/execution/database.cpp')
}
if ($Target -eq 'arithmetic64-test') { $sources = @('src/common/error.cpp') }
if ($Target -eq 'decimal-test') { $sources = @('src/common/error.cpp') }
if ($Target -eq 'aggregate-semantic-test') { $sources = @('src/common/error.cpp','src/sql/lexer.cpp','src/sql/parser.cpp','src/catalog/catalog.cpp') }
if ($Target -in @('batch-test','storage-test','buffer-test','journal-test')) { $sources = @('src/common/error.cpp','src/storage/page.cpp','src/storage/file_io.cpp','src/storage/page_file.cpp','src/storage/buffer_pool.cpp','src/storage/heap.cpp') }
$entry = switch ($Target) {
    'compile' { 'src/server/compile_main.cpp' }
    'database' { 'src/server/database_main.cpp' }
    'database-test' { 'tests/database_contract.cpp' }
    'optimizer-test' { 'tests/optimizer_contract.cpp' }
    'heap-test' { 'tests/heap_catalog_contract.cpp' }
    'arithmetic64-test' { 'tests/arithmetic64_probe.cpp' }
    'batch-test' { 'tests/write_batch_contract.cpp' }
    'storage-test' { 'tests/storage_contract.cpp' }
    'buffer-test' { 'tests/buffer_contract.cpp' }
    'journal-test' { 'tests/journal_probe.cpp' }
    'aggregate-semantic-test' { 'tests/aggregate_semantic_contract.cpp' }
    'decimal-test' { 'tests/decimal_contract.cpp' }
}
$outputName = switch ($Target) {
    'compile' { 'minisql_compile.exe' }
    'database' { 'minisql_database.exe' }
    'database-test' { 'database_contract.exe' }
    'optimizer-test' { 'optimizer_contract.exe' }
    'heap-test' { 'heap_catalog_contract.exe' }
    'arithmetic64-test' { 'arithmetic64_probe.exe' }
    'batch-test' { 'write_batch_contract.exe' }
    'storage-test' { 'storage_contract.exe' }
    'buffer-test' { 'buffer_contract.exe' }
    'journal-test' { 'journal_probe.exe' }
    'aggregate-semantic-test' { 'aggregate_semantic_contract.exe' }
    'decimal-test' { 'decimal_contract.exe' }
}
Push-Location $root
try {
    New-Item -ItemType Directory -Force -Path 'bin' | Out-Null
    $arguments = @('-std=c++20','-O0','-Wall','-Wextra','-Werror','-static','-I','include','-I',$includePath)
    $arguments += $sources
    $arguments += $entry
    if ($Target -eq 'database') { $arguments += '-lshell32' }
    $arguments += @('-o', (Join-Path 'bin' $outputName))
    & $Compiler @arguments
    if ($LASTEXITCODE -ne 0) { throw "C++ build failed: $LASTEXITCODE" }
    Write-Output "Built bin/$outputName"
} finally {
    Pop-Location
}
