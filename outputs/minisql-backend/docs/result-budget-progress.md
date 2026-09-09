# 结果行预算实现进度

日期：2026-09-09。对应 EXT-SYS-006 和 X25，当前为部分实现，不表示 X25 全量验收通过。

## 已实现

- 新增 `MINISQL_MAX_RESULT_ROWS`，在 C++ 执行层限制单条语句最终返回的行数。
- 超过预算时返回 ExecutionError 5001，错误消息为 `Result row budget exceeded`。
- LIMIT 和聚合等已经缩小结果集的语句按最终结果检查，不会按中间节点行数误判。
- HTTP capabilities 新增 `maxResultRows` 数值字段。

## 验证

- `node tests/result-budget-smoke.mjs`：7 项检查通过。
- 覆盖无 LIMIT 超限拒绝、LIMIT 合规、聚合单行和 LIMIT 1 组合。
- Release 构建通过。

## 未完成

- 预算在语句执行完成后检查，尚未在 Project/Sort/Aggregate 内部达到阈值时立即停止扫描。
- 结果集流式返回、分块传输和客户端背压仍未接入。
- 完整资源压力测试和 X25 全量验收仍待完成。
