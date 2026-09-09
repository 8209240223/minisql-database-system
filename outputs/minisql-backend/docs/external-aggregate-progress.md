# 外部聚合实现进度

日期：2026-09-09。对应 EXT-SQL-006 和 EXT-SYS-006，当前为部分实现，不表示 X06 或 X25 全量验收通过。

## 已实现

- 聚合输入先转换为包含分组键和各聚合参数的 JSON 记录。
- 记录数超过 `MINISQL_AGGREGATE_MEMORY_ROWS` 时，复用外部排序的 JSONL run、sessionId、查询/排序序号和 FNV-1a 校验机制。
- 按分组键完成外部排序后，顺序合并相邻记录，逐组计算 COUNT、SUM、MIN、MAX 和 AVG 状态。
- 保留无 GROUP BY 空输入的单行聚合结果、NULL 跳过规则、DECIMAL 精确 AVG、FLOAT 有限值检查、分组数量和状态载荷预算。
- run 文件在查询成功或异常时清理。

## 验证

- `node tests/external-aggregate-smoke.mjs`：8 项检查通过。
- 覆盖低内存阈值多 run 外部聚合、与内存聚合结果一致、NULL 分组、COUNT/SUM/MIN/MAX/AVG、临时目录和 run 清理。
- 既有聚合语法 79 项、计划 75 项、执行 95 项、AVG 83 项检查继续通过。
- Release 构建和 HTTP 回归通过。

## 未完成

- 输入记录仍先物化在内存中，不是从扫描算子直接流式写入 run。
- 查询取消、跨进程恢复、完整内存预算和输出流式返回尚未接入。
- 完整 FLOAT 聚合组合、资源压力测试和 X06/X25 全量验收仍待完成。
