# 流式结果实现进度

日期：2026-09-09。对应 EXT-SYS-006、X25，当前为部分实现，不表示 X25 全量验收通过。

## 已实现

- 新增 `POST /api/execute/stream` 和 `POST /api/sessions/:sessionId/execute/stream`。
- 流式接口只接受 `SELECT` 或 `EXPLAIN`，在执行前拒绝 `INSERT`、`UPDATE`、`DELETE` 等写语句，避免“拒绝响应已返回但写入已经发生”。
- 响应使用 NDJSON：首帧为 `meta`，中间帧为带序号的 `row`，末帧为 `complete`；异常返回 `error` 帧。
- 每个帧独立调用 `res.write`，当 TCP 写缓冲区返回背压时等待 `drain`；客户端断开后停止继续写入，已有会话取消文件和临时文件清理机制继续生效。
- HTTP capabilities 暴露 `streamingResults`、`streamingFormat=ndjson` 和 `streamingReadOnly`。

## 验证

- `node tests/x25-stream-http.mjs`：12 项检查通过。
- 覆盖 NDJSON 元数据/行/完成帧、结果顺序、写语句前置拒绝、响应断开后的后续查询和背压写入路径。

## 未完成

- 当前 C++ 执行器仍先把查询结果收集到内存，再由 bridge 分帧发送；尚未实现执行器级迭代器、逐行跨进程协议和客户端真正的生产者-消费者背压。
- 尚未实现磁盘空间不足模拟、超大单分组、流式 HashJoin/HashAggregate 和完整 X25 资源预算验收。
