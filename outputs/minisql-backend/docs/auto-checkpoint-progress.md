# 自动检查点实现进度

日期：2026-09-09。对应 EXT-SYS-003 和 X22，当前为部分实现，不表示 X22 全量验收通过。

## 已实现

- 新增 `MINISQL_AUTO_CHECKPOINT_WRITES`，按成功提交的写语句数量触发自动 CHECKPOINT。
- 新增 `MINISQL_AUTO_CHECKPOINT_WAL_BYTES`、`MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES`、`MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO` 和 `MINISQL_AUTO_CHECKPOINT_INTERVAL_MS`。
- 自动策略在成功提交后统一评估；满足多个条件时一次 CHECKPOINT，并在统计接口中记录所有触发原因。
- WAL 阈值使用本次提交生成的已同步日志大小累计。当前提交实现会在数据页恢复完成后立即截断 `.wal`，因此 `walBytes` 是实际文件大小，`pendingAutoCheckpointWalBytes` 是自动策略的累计依据。
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
- **WAL 记录内嵌 LSN 二进制格式与累积多提交日志**：`commitWriteBatch` 不再逐提交截断 `.wal`，而是**追加**独立提交扩展（日志扩展头内嵌提交序号 seq、事务 id、起始 LSN、结束 LSN、恢复起点；提交标记独立页面内嵌相同 seq），使多条提交在 `.wal` 中累积。`recoverJournal` 支持**非零截止位置重做**（优先从 `.ckpt` 的 `walCutoffBytes` 跳过已落盘前缀，仅重做其后已提交扩展），并以提交标记判定已提交/未提交尾部（未提交尾被丢弃）。每次成功提交自增 `committedSequence`（LSN 语义）。
- **LSN 跨重启单调**：新增修复——`recoverJournal` 重做后把所达最后 `committedSequence` 持久化回 `.ckpt`，保证干净重启不再回退/复用旧 LSN；无 `.ckpt` 的纯日志重放也会推进提交序号。
- **自动/调度检查点版本一致性**：`evaluateAutoCheckpoint`（提交事件驱动）与后台调度线程一致，均透传 `catalogVersion_`/`indexVersion_` 落 `.ckpt`，保证三种触发路径（显式、事件、后台）的版本元数据一致。
- 新增隔离契约测试 `page_file_wal_lsn_contract.cpp`（`.tools/harness`，`build-wal-lsn.ps1`）：覆盖累积多提交后 `.wal` 持续增长且不逐提交截断、提交序号自增、无 `.ckpt` 恢复后 LSN 连续并单调递增、**非零截止位置重做**（主文件回退后仅补其后已提交扩展且 LSN=2）、重做幂等、恢复后 `.ckpt` LSN 推进，以及**后台调度线程检查点核心动作**（累积日志上 checkpoint 清空期刊/重置截止/持久化 LSN/干净重开稳定）。

### 补齐 WAL 头字段（事务 id 与结束 LSN，2026-09-10）
- 对照任务书 B2 设计范围 3，WAL 扩展头此前只写入提交序号 seq/起始 LSN/恢复起点，缺 **事务 id** 与 **结束 LSN** 两字段（`offset 64` 曾恒为 0、`offset 80` 曾重复写起始 LSN）。现已补齐：
  - `offset 64` = 事务 id（`txSequence_` 分配器随每次 `beginWriteBatch` 递增，写入 `WriteBatch::txId`）。
  - `offset 80` = 结束 LSN = 起始 LSN + 本次扩展字节数（`2*(N+1)*kPageSize`，N 为页记录数）。
  - 恢复逻辑不依赖这两字段（仍以 seq + 提交标记判断），故对旧 `.wal` 格式向后兼容。
- `page_file_wal_lsn_contract.cpp` 新增扩展头字段断言：两次提交的 seq/txId/startLsn/endLsn/recoveryStart 均一一校验。隔离编译 + 运行从 23 项增至 **36 项全绿**；检查点契约 21 项、页级 B+ 树契约 496 项均无回归。

### 待验证/接入
- 新路径的跨进程故障注入与 node 全量回归（`auto-checkpoint-smoke`/`x22-fault-injection`/`journal-process`，以及后台调度线程启用路径）需在有 vcpkg + node 环境验证；`database.cpp` 改动在本隔离环境无法编译断言，需在完整构建中确认。

## 验证

- `node tests/auto-checkpoint-smoke.mjs`：62 项检查通过。
- 覆盖写语句累计、WAL 大小、脏页数量、脏页比例、时间窗口、显式 CHECKPOINT 重置、事务提交统计和重启后数据一致。
- `node tests/x22-fault-injection.mjs`：30 项检查通过；覆盖五个提交/恢复阶段，验证故障后恢复、重复恢复和提交前后数据边界。
- Release 构建通过。

## 未完成

- 当前为提交事件驱动检查，不在后台线程中对活动事务强制执行 CHECKPOINT；在线后台调度、工作台展示和全项目 X22 组合验收仍待完成。
- 现有 WAL 在单次提交完成后立即截断，尚未改为保留多个提交日志直到独立检查点，因此不能据此宣称已实现完整 ARIES/WAL 截止位置语义。
- 迁移、在线备份、外部执行资源和 X01-X27 全量验收仍按各自 progress 文档推进。
