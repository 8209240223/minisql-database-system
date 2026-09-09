# 服务端查询取消实现进度

日期：2026-09-09。对应 EXT-SYS-006 和 X25，当前为部分实现，不表示 X25 全量验收通过。

## 已实现

- C++ `Database` 支持 `MINISQL_CANCEL_FILE`，在查询入口、扫描、HashJoin/NestedLoopJoin、聚合和外部排序过程中检查取消令牌。
- 新增 `Cancelled = 5002` 错误码，取消响应类型为 `CancelledError`。
- HTTP bridge 为每个 session 创建独立取消文件，并提供 `POST /api/sessions/:id/cancel`。
- 取消请求只在对应 session 有活动请求时生效；空闲会话返回 409。
- 取消不会终止 session 进程，取消后的会话可以继续执行查询。
- 外部排序和外部聚合的 run/meta 文件在取消异常路径中由 RAII 清理。
- 能力接口新增 `cancellation`、`cancellationMode: cancel-file` 和 `cancelledErrorCode: 5002`。

## 验证

- `node tests/cancel-smoke.mjs`：20 项检查通过。
- 覆盖空闲取消拒绝、查询中取消、5002 错误、会话存活、后续查询可用和临时文件清理。
- `node tests/observability-http.mjs`：42 项检查通过。
- Release 构建通过。

## 未完成

- 一次性 HTTP 请求和 session 请求均已绑定取消文件；所有复杂算子仍未覆盖，网络断开后的提交状态仍需要客户端按“未知”处理。
- 取消令牌是本地文件轮询，不是进程级共享内存或网络取消协议；跨进程恢复和更细粒度算子中断仍待实现。
- 执行器级流式迭代、完整资源预算和 X25 全量压力验收仍待完成；传输层 NDJSON 流式结果见 streaming-progress.md。
