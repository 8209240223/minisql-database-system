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

## 验证

- `node tests/auto-checkpoint-smoke.mjs`：62 项检查通过。
- 覆盖写语句累计、WAL 大小、脏页数量、脏页比例、时间窗口、显式 CHECKPOINT 重置、事务提交统计和重启后数据一致。
- `node tests/x22-fault-injection.mjs`：30 项检查通过；覆盖五个提交/恢复阶段，验证故障后恢复、重复恢复和提交前后数据边界。
- Release 构建通过。

## 未完成

- 当前为提交事件驱动检查，不在后台线程中对活动事务强制执行 CHECKPOINT；在线后台调度、工作台展示和全项目 X22 组合验收仍待完成。
- 现有 WAL 在单次提交完成后立即截断，尚未改为保留多个提交日志直到独立检查点，因此不能据此宣称已实现完整 ARIES/WAL 截止位置语义。
- 迁移、在线备份、外部执行资源和 X01-X27 全量验收仍按各自 progress 文档推进。
