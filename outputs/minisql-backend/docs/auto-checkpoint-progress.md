# 自动检查点实现进度

日期：2026-09-10。对应 EXT-SYS-003 和 X22，当前为部分实现，不表示 X22 全量验收通过。

## 已实现

- 新增 `MINISQL_AUTO_CHECKPOINT_WRITES`，按成功提交的写语句数量触发自动 CHECKPOINT。
- 新增 `MINISQL_AUTO_CHECKPOINT_WAL_BYTES`、`MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES`、`MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO` 和 `MINISQL_AUTO_CHECKPOINT_INTERVAL_MS`。
- 自动策略在成功提交后统一评估；满足多个条件时一次 CHECKPOINT，并在统计接口中记录所有触发原因。
- WAL 阈值使用本次提交生成的已同步日志大小累计；多提交日志保留到独立 checkpoint，`walBytes` 表示当前日志文件大小，`pendingAutoCheckpointWalBytes` 表示自动策略的累计依据。
- 脏页阈值使用提交前写批次中的唯一暂存页数；比例以 BufferPool 容量为分母并限制在 0 到 1，用于避免 `heap.flush()` 提前清除 dirty 标志后阈值失效。
- 显式 `CHECKPOINT`、非事务写入和事务 `COMMIT` 都会重置累计量；事务内多条成功写语句在提交时一次计数，回滚不会计入。
- `statistics()` 返回阈值配置、实际 `walBytes`、当前 `dirtyPages`/`dirtyPageRatio`、累计量、上次检查点时间和自动触发原因。
- 增加 `MINISQL_CRASH_AT` 跨进程故障注入入口，支持 `prepared`、`published`、`applied-page`、`data-synced` 和 `checkpointed` 阶段，进程以 77 退出。
- HTTP capabilities 暴露各自动检查点阈值、`committed-journal-bytes` 基准和 `after-successful-commit` 评估时机。

## B2 追加（持久化检查点记录 + WAL 截止位置/提交序号元数据）

- `PageFile` 新增持久化检查点记录 `.ckpt`（4096 字节、带校验），记录 **WAL 截止位置 `walCutoffBytes`、脏页水位 `dirtyWatermark`、目录版本、索引版本、提交序号（LSN 语义）与时间戳**；构造时加载并校验，损坏即明确拒绝（`STORAGE_CORRUPTION`）。
- `commitWriteBatch` 每次成功提交自增 `committedSequence`；`checkpoint()` 将其与脏页水位（= 已落盘页数上限）、由上层透传的目录/索引版本写入 `.ckpt` 并落盘。
- `Database::checkpoint()`（显式与计划路径）透传 `catalogVersion_`/`indexVersion_`；`statistics()` 暴露 `committedSequence`、`dirtyWatermark` 与完整 `checkpointRecord`（截止位置/水位/版本/序号/时间戳）。
- 新增隔离契约测试 `page_file_checkpoint_contract.cpp`（`.tools/harness`）：覆盖全新无记录、提交后序号自增、检查点落盘版本/水位/截止位置、重开后恢复、损坏 `.ckpt` 拒绝。隔离编译 + 运行 21 项全绿；页级 B+ 树契约 496 项无回归。

### 后续增量（本轮已实现部分）
- **后台检查点调度线程**：`Database` 新增 `std::recursive_mutex mu_` 串行化全部公共入口（execute/compile/statistics/checkpoint/catalog/diagnostics/indexInspect/configureBuffer/executeScript/runCorrelatedSubquery）与后台线程，杜绝与语句执行竞争。`MINISQL_BACKGROUND_CHECKPOINT_MS` 环境变量设为正值时启动调度线程；线程按该周期评估五类阈值（writes/wal-bytes/dirty-pages/dirty-ratio/interval），**仅当事务空闲（Idle）且无写批时执行 `file_->checkpoint()`**，绝不在活动事务提交点前截断未提交日志；命中阈值但事务忙碌时计入 `deferredReasons` 延后执行。析构时置停止标志、通知并 join 线程。`statistics()` 的 `backgroundScheduler` 暴露 enabled/intervalMs/lastEvaluateMs/lastRunMs/deferredReasons。默认（未设变量）不启动线程，行为与原先完全一致。
- **WAL 记录内嵌 LSN 二进制格式与累积多提交日志**：`commitWriteBatch` 不再逐提交截断 `.wal`，而是**追加**独立提交扩展（日志扩展头内嵌提交序号 seq、事务/批起始 count、起始 LSN、恢复起点；提交标记独立页面内嵌相同 seq），使多条提交在 `.wal` 中累积。`recoverJournal` 支持**非零截止位置重做**（优先从 `.ckpt` 的 `walCutoffBytes` 跳过已落盘前缀，仅重做其后已提交扩展），并以提交标记判定已提交/未提交尾部（未提交尾被丢弃）。每次成功提交自增 `committedSequence`（LSN 语义）。
- **LSN 跨重启单调**：新增修复——`recoverJournal` 重做后把所达最后 `committedSequence` 持久化回 `.ckpt`，保证干净重启不再回退/复用旧 LSN；无 `.ckpt` 的纯日志重放也会推进提交序号。
- **自动/调度检查点版本一致性**：`evaluateAutoCheckpoint`（提交事件驱动）与后台调度线程一致，均透传 `catalogVersion_`/`indexVersion_` 落 `.ckpt`，保证三种触发路径（显式、事件、后台）的版本元数据一致。
- 新增隔离契约测试 `page_file_wal_lsn_contract.cpp`（`.tools/harness`，`build-wal-lsn.ps1`）：覆盖累积多提交后 `.wal` 持续增长且不逐提交截断、提交序号自增、无 `.ckpt` 恢复后 LSN 连续并单调递增、**非零截止位置重做**（主文件回退后仅补其后已提交扩展且 LSN=2）、重做幂等、恢复后 `.ckpt` LSN 推进，以及**后台调度线程检查点核心动作**（累积日志上 checkpoint 清空期刊/重置截止/持久化 LSN/干净重开稳定）。隔离编译 + 运行 23 项全绿；原检查点契约 21 项无回归。

### 合并后验证（2026-09-10）
- Windows Release 主工程构建通过，`database.cpp` 的后台调度和版本透传改动已纳入真实引擎。
- `node tests/auto-checkpoint-smoke.mjs`：62 项通过。
- `node tests/x22-fault-injection.mjs`：30 项通过，覆盖 `prepared`、`published`、`applied-page`、`data-synced`、`checkpointed` 五个阶段；恢复路径不会把正常重启误判为当前故障注入点。
- `node tests/journal-process.mjs`：88 项通过，覆盖提交边界、重做、重复恢复、损坏、身份、锁和旧格式迁移。
- `node tests/fuzz-state-machine-long-run.mjs`：2 个固定种子各 512 步通过，未出现崩溃、超时或资源限制错误。

## 验证

- `node tests/auto-checkpoint-smoke.mjs`：62 项检查通过。
- 覆盖写语句累计、WAL 大小、脏页数量、脏页比例、时间窗口、显式 CHECKPOINT 重置、事务提交统计和重启后数据一致。
- `node tests/x22-fault-injection.mjs`：30 项检查通过；覆盖五个提交/恢复阶段，验证故障后恢复、重复恢复和提交前后数据边界。
- Release 构建通过。

## 尚未闭合

- 后台调度已实现并通过核心回归，但在线备份、活动事务边界的更高压力组合和工作台端到端展示仍需独立验收。
- 当前 WAL/检查点实现满足本项目的页级恢复和截止位置测试，不等同于完整 ARIES 实现；更复杂的并发恢复语义仍不在本轮承诺内。
- 迁移、在线备份、外部执行资源和 X01-X27 全量验收仍按各自 progress 文档推进。
