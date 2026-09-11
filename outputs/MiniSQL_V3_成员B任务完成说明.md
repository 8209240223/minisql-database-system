# MiniSQL V3 成员 B 任务完成说明（基础版）

日期：2026-09-10
范围调整：按用户 2026-09-10 要求，本次只要求实现和验证基础功能；复杂并发压力、超大性能曲线、长时间资源趋势和 MVCC 不作为本次基础交付门槛。

## 一、完成范围

### X20 页级 B+ 树与索引

- 页级 B+ 树默认主路径、元页/节点页、根到叶遍历、分裂、借位/合并、重启恢复、结构 inspect。
- `index-smoke.mjs`：32 + 44 + 47 项通过。
- 1000 / 5000 / 10000 行索引规模回归和性能曲线通过，树高 2/3/3，页数 33/162/323。

### X21 事务、原子性与保存点

- DDL/DML 事务、提交、回滚、失败回滚、HTTP 常驻会话和数据库级排他事务锁。
- `SAVEPOINT`、`RELEASE SAVEPOINT`、`ROLLBACK TO SAVEPOINT`，支持 DML、DDL 和索引恢复。
- `transaction-process.mjs` 74 项、`transaction-savepoint.mjs` 7 项通过。
- 写批页数预算可配置，超限返回明确错误并回滚；`transaction-overflow.mjs` 5 项通过。

### X22 WAL、检查点与恢复

- 整页 WAL、提交边界、重做、重复恢复、损坏和身份校验、显式/自动/后台 CHECKPOINT、持久化 checkpoint 记录、累积 WAL/LSN、非零截止位置重做。
- `journal-process.mjs` 88 项、`auto-checkpoint-smoke.mjs` 62 项、`x22-fault-injection.mjs` 30 项通过。

### X23 并发控制（声明方案）

- 多逻辑会话、数据库级两阶段锁、锁等待/超时、关闭回滚和会话上限。
- `session-process.mjs` 51 项、`session-http.mjs` 28 项、`multi-session-http.mjs` 44 项通过。
- 当前方案明确不提供 MVCC。

### X25 外部排序、取消、资源与流式

- 外部排序/聚合、结果行预算、临时文件清理、取消令牌和会话级多帧流。
- `external-sort-smoke.mjs` 13 项、`external-aggregate-smoke.mjs` 8 项、`result-budget-smoke.mjs` 7 项、`cancel-smoke.mjs` 20 项通过。
- `x25-stream-http.mjs` 18 项、`x25-row-stream-contract.mjs` 15 项、`session-stream-process.mjs` 9 项通过。

### X26 备份、恢复与迁移

- v2 全量备份、v1 迁移、v3 增量链、v4 在线一致性快照、备份链深度、页校验和、原子恢复、恢复回滚目录与失败自动还原。
- 恢复采用逐页 materialize，避免先构造完整内存页缓冲。
- `backup-smoke.mjs` 56 项、`backup-online-smoke.mjs` 22 项通过。

## 二、验证门禁

以下命令已全部通过：

```powershell
powershell -ExecutionPolicy Bypass -File ./outputs/run-minisql-tests.ps1 -Suite all
```

该套件覆盖编译前端、执行、事务、保存点、事务溢写、WAL、检查点、索引、性能曲线、备份恢复、HTTP、会话、取消、流式、权限和工作台构建。

## 三、明确不纳入基础版的高级项

- MB 级更大规模索引性能压力。
- 长时间并发恢复压力和多小时资源趋势。
- MVCC、行级锁和更细粒度隔离。
- 排序/聚合算子的完全外存逐行生产与更细粒度提前停止。
- 在线备份/恢复的超高并发压力组合。

这些项目保留为后续增强方向，不作为本次成员 B 基础版完成判据。

## 四、结论

成员 B 基础功能已完成并进入可复跑回归链；本地和远程 `main` 已同步。
