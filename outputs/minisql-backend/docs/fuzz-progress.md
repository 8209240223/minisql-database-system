# 固定种子与差分测试

日期：2026-09-10。对应 EXT-QA-001、X27，目前为部分完成。

## 一、测试链路

1. `tests/fuzz-model.mjs` 生成受当前表结构约束的 SELECT 模型，覆盖 INT 算术、比较、NOT/AND/OR、Unicode 字符串、DISTINCT、多键排序、LIMIT/OFFSET。整数和除数有界，不制造参考数据库与 MiniSQL 的溢出或除零语义差异。
2. `tests/fuzz-differential.mjs` 使用真实 `minisql_database.exe` 和 Node `node:sqlite` 分别执行同一数据集和查询。SQLite 只作测试参考，不替代产品的 C++ 执行器或存储。
3. 数据集包括重复行、负数、中文和含单引号的字符串；排序键覆盖全部输出，避免相同排序键下的合法排列差异被误判。
4. 每个模型附加四种受控变异：非法字符、缺分号、不存在的列，以及仍然合法的注释/关键字大小写变化。合法变异不要求报错。
5. 每条 SQL 由新子进程重开隔离数据库。单次限制 5 秒和 8 MiB 输出；统计结果差异、错误接受、错误拒绝、错误位置、崩溃、超时、资源限制及测试框架错误。
6. 遇到查询结果差异或错误拒绝时，仅在既有合法语法模型中删除条件、简化表达式及移除修饰项，最多探测 64 次。缩减后再次确认相同故障分类，保存原用例和缩减结果，不宣称得到全局最小 SQL。
7. 每次运行创建独立 `tests/artifacts/fuzz-*` 目录，保存 fixture.sql、corpus.sql、report.json，以及适用时的 failure-N.json。报告记录种子、参数、语料摘要、生成器摘要、C++ 可执行文件摘要和参考引擎版本。

## 二、运行方式

在后端目录执行 `node tests/fuzz-model-contract.mjs`、`node tests/fuzz-process-contract.mjs` 和 `node tests/fuzz-differential.mjs`。

默认种子 20260908、100 个模型，每模型 1 次差分和 4 次变异，共 500 次检查。环境变量 FUZZ_SEED 接受 UINT32，FUZZ_CASES 接受 1 至 1000。测试依赖已构建的后端可执行文件和 Node 24 的 `node:sqlite`。

SELECT 语料重放需保留报告中的版本和生成器，使用同一 `FUZZ_SEED`、`FUZZ_CASES`，并比对 `corpusSha256`。状态机失败样本直接使用报告目录中的 `failure-*.json`，设置 `FUZZ_STATE_REPLAY=<artifact>` 即可按保存的完整 `program` 重放；仅种子相同但生成器版本不同，不保证语料相同。

## 三、验证结果

生成器及缩减器 10 项检查通过，包括同种子确定性、不同种子、条件缩减、预算停止、保留故障、输入不变，以及默认语料覆盖 1 至 4 个条件。

子进程隔离 7 项检查通过，实际启动测试子进程验证超时、输出超限、异常退出、非法 JSON、退出码与 JSON 状态不一致、正常结果及正常 SQL 错误。

最终生成器的种子 20260908、100 模型运行已通过 500 次检查，所有故障计数为零。语料 SHA-256 为 `1487bf24d633a37336f0ed789e5b58b604287c2a736f51ac339623d3f1f1cc52`。这表示本次样本通过，不表示无缺陷或覆盖完整 SQL。

## 四、剩余范围

通用差分语料仍主要是 SELECT；独立状态机语料已经覆盖 UPDATE、DELETE、多行 INSERT、JOIN、NULL、事务、索引和受控负向 DDL 探针，崩溃恢复组合另由固定种子故障注入脚本覆盖。没有覆盖尚未实现的语法，也没有做无限输入或多小时生产压力测试；固定规模的高压溢写与资源泄漏回归见第八节。

缩减器的逻辑已用可控故障谓词测试；本批实际数据库没有出现结果差异，因此没有真实引擎缺陷的自动缩减案例。错误变异仅保存输入与分类，尚未自动缩减。错行错列的精确断言目前集中在注入的非法字符，其余诊断只检查阶段类型和有效位置。

参考引擎仅用于共同支持子集。测试生成器不能替代完整 Catalog 驱动的随机 DDL/DML 状态机。EXT-QA-001 保持部分完成。

## 五、X27 DDL/DML 状态机语料（2026-09-09 补）

### 新增内容

1. `tests/fuzz-state-machine.mjs` 提供构造即合法的 DDL/DML 状态机（CREATE/INSERT/UPDATE/DELETE/SELECT/事务/索引 + 受控负向探针），并暴露 `generateProgram`、`renderProgram`、`validateProgram`、`chunkProgram`、`minimizeProgram` 等可复用接口。
2. 新增 `tests/fuzz/generate-corpus.mjs`：按固定种子生成并持久化长期回归语料：
   - `fixture.sql` —— 表结构与初始数据；
   - `corpus-select-<seed>.sql` —— 由 `fuzz-model.mjs` 生成的 SELECT 语料；
   - `corpus-state-<seed>.sql` —— 由状态机生成的 DDL/DML 语料；
   - `manifest.json` —— 记录种子、样本数、各语料 SHA-256、生成器 SHA-256、fixture SHA-256 与生成时间，满足"固定种子长跑、CI 回归和失败样本重放"的可审计要求。
3. 状态机程序在持久化前用 `validateProgram` 重放校验，杜绝悬空表引用、缺列、重复键或不平衡事务，确保"构造即合法"；差分失败时另写 `failure-*.json` 保存完整程序、故障分类和缩减结果。

### 运行方式

```powershell
cd outputs\minisql-backend
node tests/fuzz/generate-corpus.mjs                      # 默认种子 20260908,42,7
$env:FUZZ_SEEDS = '20260908,123' ; node tests/fuzz/generate-corpus.mjs
```

### 验证结果

固定种子 `20260908 / 42 / 7` 各生成 100 条 SELECT 语料与 96 步状态机程序；`validateProgram` 全部通过，`manifest.json` 已生成。`reproducibility-contract.mjs` 在两个临时目录逐文件比较 SQL 字节并归一化比较 Manifest，确认同一种子、同一生成器版本可逐字节复现。

状态机差分脚本会优先选择 `build/windows/Release/minisql_database.exe`，也支持通过 `MINISQL_DATABASE_EXE` 指定引擎路径。当前固定种子 `20260908,42,7`、每种子 96 步的真实 C++ 回归已通过：64 个可校验结果，结果差异、错误接受、错误拒绝、错误位置、崩溃、超时和资源限制均为零。

## 六、X27 长跑回归与缺陷修复

### 本轮新增验证

1. `tests/fuzz-state-machine-long-run.mjs` 关闭负向 DDL 探针，使用固定种子 `20260908`、`42`，每个种子执行 512 步真实 C++ session 状态机，覆盖持续 CREATE/INSERT/UPDATE/DELETE/SELECT、事务、NULL、JOIN、复合索引和索引重建组合。
2. 长跑每次创建独立数据库和 session，进程预算为 180 秒，要求 `wrongResult`、`wrongAccept`、`wrongReject`、`crash`、`timeout` 均为 0；本机实际通过 2 个种子 × 512 步。
3. 长跑首次发现并修复规划器字面量类型覆盖缺陷：日期文本中的 `E` 和布尔文本 `TRUE` 中的 `E` 不再触发浮点字面量分支；`tests/date-literal-process.mjs` 以 13 项检查覆盖 DATE/BOOL 类型、带索引 UPDATE、DECIMAL 持久化结果。

### 可复跑命令

```powershell
cd outputs\minisql-backend
node tests/date-literal-process.mjs
node tests/fuzz-state-machine-long-run.mjs
```

长跑结果 artifact 保留在 `tests/artifacts/fuzz-state-*`，报告包含可执行文件 SHA-256、生成器 SHA-256、固定种子、步数和分类计数；若后续出现故障，仍可通过 `FUZZ_STATE_REPLAY` 重放完整程序。

## 七、状态机与崩溃恢复组合

`tests/fuzz-state-machine-crash-recovery.mjs` 使用固定种子 `20260908`、40 步 DDL/DML/事务/索引状态机作为基线，然后在独立数据库上对 `prepared`、`published`、`applied-page`、`data-synced`、`checkpointed` 五个提交阶段分别注入故障。每个案例都重启 C++ 引擎，检查 setup 行未丢失、事务恢复结果是目标事务的稳定前缀、重复读取一致，并在恢复后继续提交一行数据。

本机 5 个阶段全部通过。该测试输出 `report.json`，记录固定种子、状态机步数、可执行文件 SHA-256、生成器 SHA-256、基线 SQL SHA-256、每个故障阶段恢复的事务行数和恢复后的最终行数。

## 八、X27 高压与资源趋势采样

`tests/fuzz-state-machine-pressure.mjs` 在独立数据库上使用固定种子 `20260908`、`42`、`7`、`11`，每个种子执行 512 步状态机，并预先写入 4096 条压力数据。测试把排序和聚合内存预算压到 64 行，强制触发外部 run 与多 run 合并，同时检查状态机执行后的临时文件、外部溢写临时文件和会话关闭后的临时文件均为空。

本机 4 个种子全部通过。每个案例记录执行耗时、数据库字节数和进程 RSS 最小/最大采样值；该证据覆盖固定的高压溢写与资源泄漏回归，但不等同于无限输入或多小时长期运行资源曲线。

## 九、重复长跑入口

新增 `tests/fuzz-state-machine-soak.mjs`，使用相同的固定种子、状态机步数和资源限制重复启动 `fuzz-state-machine-differential.mjs`，每轮仍然使用独立数据库和 C++ session。默认执行 `20260908,42` 两个种子、每种子 512 步、两轮；每轮都检查 Wrong Result、Wrong Accept、Wrong Reject、错误定位、Crash、Timeout、Resource Limit 和 harness error，最终保存 `report.json`。

短验收可以使用：

```powershell
$env:FUZZ_SOAK_ROUNDS = '1'
$env:FUZZ_SOAK_STEPS = '30'
node tests/fuzz-state-machine-soak.mjs
Remove-Item Env:FUZZ_SOAK_ROUNDS
Remove-Item Env:FUZZ_SOAK_STEPS
```

长期或夜间回归可设置 `FUZZ_SOAK_ROUNDS`、`FUZZ_SOAK_STEPS`、`FUZZ_SOAK_SEEDS` 和 `FUZZ_SOAK_TIMEOUT_MS`。该入口提供可重复的多轮证据，但本机当前只执行了短 smoke；它不替代无限输入或多小时资源趋势验收。

### 失败样本重放

`fuzz-state-machine-differential.mjs` 在 Wrong Result、Wrong Reject、错误定位、崩溃、超时或资源限制等故障发生时，向报告目录写入 `failure-<n>.json`。文件包含种子、故障分类、完整状态机 `program` 和（适用时）缩减程序。重放命令如下：

```powershell
$env:FUZZ_STATE_REPLAY = 'tests/artifacts/fuzz-state-xxxxxx/failure-1.json'
node tests/fuzz-state-machine-differential.mjs
Remove-Item Env:FUZZ_STATE_REPLAY
```

`fuzz-state-machine-replay-contract.mjs` 使用固定种子程序验证该入口能执行并检查保存的分类；真实故障 artifact 的分类不应被静默忽略。

### 边界与后续

- 状态机语料已覆盖 UPDATE/DELETE/多行 INSERT/JOIN/NULL/事务/索引与受控负向 DDL 探针；提交阶段崩溃恢复组合和固定种子高压溢写/资源泄漏回归已经通过，但无限输入、多小时长期资源趋势和在线备份边界仍未完成。
- CI（`.github/workflows/ci.yml`）已接入权限契约、CLI 契约、固定种子逐文件复现和工作台单元回归；Windows job 还会构建真实 C++ 引擎并运行 HTTP、SELECT 差分、DDL/DML 状态机差分、状态机长跑、崩溃恢复、高压资源回归、状态机重放契约和工作台真实浏览器回归。
