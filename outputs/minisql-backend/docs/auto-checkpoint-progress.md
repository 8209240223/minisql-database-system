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

### 后续增量（本轮未含）
- 真正的**后台调度线程**（当前为提交事件驱动；后台线程需与单写执行器协调，避免在活动事务提交点前截断未提交日志）。
- **WAL 记录内嵌提交序号/事务 id/起始-结束 LSN/恢复起点** 的二进制格式，以及**累积多提交日志 + 非零截止位置重做**（当前整批镜像提交后立即截断 `.wal`，故截止位置为 0；需改缓冲刷新模型使其累积安全）。
- 新路径的跨进程故障注入与 node 全量回归（`auto-checkpoint-smoke`/`x22-fault-injection`/`journal-process`）需在有 vcpkg + node 环境验证。

## 验证

- `node tests/auto-checkpoint-smoke.mjs`：62 项检查通过。
- 覆盖写语句累计、WAL 大小、脏页数量、脏页比例、时间窗口、显式 CHECKPOINT 重置、事务提交统计和重启后数据一致。
- `node tests/x22-fault-injection.mjs`：30 项检查通过；覆盖五个提交/恢复阶段，验证故障后恢复、重复恢复和提交前后数据边界。
- Release 构建通过。

## 未完成

- 当前为提交事件驱动检查，不在后台线程中对活动事务强制执行 CHECKPOINT；在线后台调度、工作台展示和全项目 X22 组合验收仍待完成。
- 现有 WAL 在单次提交完成后立即截断，尚未改为保留多个提交日志直到独立检查点，因此不能据此宣称已实现完整 ARIES/WAL 截止位置语义。
- 迁移、在线备份、外部执行资源和 X01-X27 全量验收仍按各自 progress 文档推进。
