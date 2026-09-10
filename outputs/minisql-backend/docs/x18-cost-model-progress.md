# X18 统计与成本模型 —— progress

> 成员 A 交付物（任务书 A5-①）。设计目标见任务书 A4；执行记录见 `task-A-development-plan.md` §6.9–§6.16、§6.21。

## 状态：已完成

## 已实现

- **统计信息**：表行数 / 页数；列 `distinctCount`、`nullCount`、`nullRatio`、`min` / `max`；数值列等宽直方图（≤ 16 桶）；`version = "stats-v1-histogram"`、`generatedAtMs`。
- **显式 ANALYZE**：`ANALYZE [TABLE] <name>` 刷新并把统计快照持久化到 `<db>.analyze.json`；`statistics()` 报告 `source: "analyze"` + `lastAnalyzeAtMs`；任何写语句成功后快照失效并回退 `on-demand-scan`。
- **成本模型**：SeqScan / IndexScan / Filter / Sort / Limit / Distinct / Aggregate / Join 的行数与累计成本公式，以及选择率公式——详见 [cost-model.md](cost-model.md)。
- **确定性**：固定决胜规则；相同输入 + 相同统计的重复编译产出逐字节一致的 `optimizedPlan`。
- **计划字段**：IndexScan / Join / Sort 计划节点输出 `estimatedRows` / `estimatedCost` / `statsSource`；进入 HTTP 契约与工作台展示（徽标）。
- **EXPLAIN / EXPLAIN ANALYZE**：估计来源与实际来源区分；返回实际行数 / 耗时 / 页读写；不伪造加速倍数。

## 验证

- `statistics-smoke` 11 / `statistics-histogram-smoke` 20 / `explain-smoke` 21 / `analyze-stats-smoke` 35 项通过。
- `statistics-costjoin-smoke`：成本驱动 join 选择 + 跨编译确定性断言。

## 已知局限

- join 行数估计为 `L × R`（NestedLoop 再乘 0.1），未引入连接选择率因子——属已声明的部分实现。
- `ANALYZE` 语法针对单表，但一次刷新会重算全库表统计并写入同一份快照。
