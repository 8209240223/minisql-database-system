# 外部排序实现进度

日期：2026-09-09。对应 EXT-SQL-005 和 EXT-SYS-006，当前为部分实现，不表示 X05 或 X25 全量验收通过。

## 已实现

- Sort 输入超过内存行预算时，按 `MINISQL_SORT_MEMORY_ROWS` 分块排序并写入 JSONL run。
- 使用优先队列对多个 run 执行多路归并，比较器复用原有 ORDER BY 的升降序、NULLS FIRST/LAST 和多键规则。
- 临时目录由 `MINISQL_TEMP_DIR` 配置，默认位于数据库文件旁的 `.minisql-sort`。
- run 文件名绑定 sessionId、查询序号和排序序号；每个 run 写入 `.meta.json`，记录行数、`fnv1a64` 校验和校验算法，归并前先验证元数据与内容。
- 查询成功结束或抛出异常时清理本查询创建的 run 文件。
- 内存预算范围内继续使用 `std::stable_sort`，保持既有稳定排序语义。

## 验证

- `node tests/external-sort-smoke.mjs`：13 项检查通过。
- 覆盖外部排序与内存排序结果一致、多键排序、升降序、NULLS FIRST/LAST、临时目录创建和 run 文件清理。
- Release 构建通过。

## 未完成

- Sort 当前仍消费已物化的结果集，不是流式输入，也没有查询取消和完整资源预算。
- 临时文件只记录 sessionId、查询/排序序号和校验信息；查询级 SQL 摘要、取消令牌和跨进程恢复仍未接入。
- 外部聚合、溢出页、执行器级流式迭代、服务端取消跨进程恢复和完整 X25 验收仍待完成。传输层 NDJSON 流式结果见 streaming-progress.md。
