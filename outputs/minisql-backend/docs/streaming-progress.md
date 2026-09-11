# 流式结果实现进度

日期：2026-09-10。对应 EXT-SYS-006、X25，当前为部分实现，不表示 X25 全量验收通过。

## 已实现

- 新增 `POST /api/execute/stream` 和 `POST /api/sessions/:sessionId/execute/stream`。
- 流式接口只接受 `SELECT` 或 `EXPLAIN`，在执行前拒绝 `INSERT`、`UPDATE`、`DELETE` 等写语句，避免“拒绝响应已返回但写入已经发生”。
- 响应使用 NDJSON：首帧为 `meta`，中间帧为带序号的 `row`，末帧为 `complete`；异常返回 `error` 帧。
- 每个帧独立调用 `res.write`，当 TCP 写缓冲区返回背压时等待 `drain`；客户端断开后停止继续写入，已有会话取消文件和临时文件清理机制继续生效。
- HTTP capabilities 暴露 `streamingResults`、`streamingFormat=ndjson` 和 `streamingReadOnly`。
- 新增执行器级 `RowStream` 接口（`next`/`cancel`/`close`/`resourceUsage`），以及 `Filter`/`Project`/`Sort`/`Limit`/`Distinct` 流式算子和预算装饰器（`executor.{hpp,cpp}`）；有界外部排序写入带校验的 JSONL run 后多路归并，15 项 executor 契约检查通过（`build-executor.ps1`）。
- `MINISQL_EXECUTOR=stream` 时，database.cpp 单表读路径（`SeqScan`/`Filter`/`Project`/`Sort`/`Limit`/`Distinct`）改为 RowStream 组合执行，并在顶层施加行数/临时文件字节/sort run 数/聚合状态数预算（`MINISQL_MAX_RESULT_ROWS`/`MINISQL_RESULT_TEMP_BYTES`/`MINISQL_RESULT_SORT_RUNS`/`MINISQL_RESULT_STATES`），达预算立即停止并关闭下游释放临时文件/pin；`executor.cpp` 已接入 `minisql_execution` 构建目标。默认关闭，保留既有物化路径不变。

## 逐行跨进程流式与消费者背压

- `Database::streamQuery(...)`：单条 SELECT 编译后若具备流式资格则构建 RowStream 并施预算，随后逐行回调 `onMeta`/`onRow`/`waitBackpressure`（持有递归互斥锁），替代原先先物化到 `rows` 的路径。
- `minisql_database` 会话新增 `stream` 操作：逐行输出 `meta`/`row`/`complete`/`error` 帧，并在每个 `row` 后读取跨进程控制帧（`{"ack":id}` 继续、`{"cancel":id}` 取消），把下游消费节奏反压给 C++ 端。
- `SessionProcess` 会话进程（session-process.mjs）新增 `stream()` 方法：对每个非终帧 `await onMessage`，仅当 `row` 帧的 HTTP 写背压 promise 完成后才写 `ack`，实现真正的生产者-消费者背压；短路写断开会话或触发取消按 5002 结束。
- bridge `/api/execute/stream` 接入逐行路径（原为物化后再分帧）：`createNdjsonWriter` 用 `res.write`+`drain` 表达背压，断开会话写 cancelFile 触发取消；manifest/审计保持既有语义。

## 验证

- `node tests/x25-stream-http.mjs`：12 项检查通过。
- `node tests/x25-stream-http.mjs`：18 项检查通过，覆盖普通 HTTP 流式帧、会话级 C++ 多帧流式、结果顺序、写语句前置拒绝、响应断开后的后续查询和背压写入路径。
- `node tests/session-stream-process.mjs`：9 项通过，直接用 session 进程验证 `meta/row/complete/error` 帧和 LIMIT 提前停止。

## 未完成

- 当前 C++ 执行器仍先把查询结果收集到内存，再由 bridge 分帧发送；尚未实现执行器级迭代器、逐行跨进程协议和客户端真正的生产者-消费者背压。
- 尚未实现磁盘空间不足模拟、超大单分组、流式 HashJoin/HashAggregate 和完整 X25 资源预算验收。

## 执行器接口增量

- 新增 `include/minisql/execution/executor.hpp` 中的 `RowStream` 抽象接口，定义 `next`、`cancel`、`close` 和 `resourceUsage`。
- 新增 `ScanRowStream`，用于扫描层的资源计量和取消点；当前作为接口层落地，尚未把所有算子整体迁移到逐行流。
- 新增 `FilterRowStream`、`ProjectRowStream`、`LimitRowStream`，并提供 `Database::openRowStream` 打开简单 `SeqScan -> Filter -> Project -> Limit` 链。
- HTTP 层 `queryResult` 现在回传最后一个执行节点的 `resourceUsage`，`x25-row-stream-contract.mjs` 15 项通过。
- `Limit` 在子计划支持 `RowStream` 时直接按 offset/limit 读取并提前停止；普通单表 `Project` 也会优先通过 `openRowStream` 执行。
- 新增 `MaterializedRowStream`，`Sort`、`Aggregate`、`Distinct` 结果可以通过统一 `RowStream` 接口逐行消费；它们仍先物化结果，但后续 HTTP 逐行输出可以直接复用该接口。
- 新增 C++ `Database::executeStreaming` 和会话操作 `executeStream`：会话进程按行发送 `meta/row/complete` 帧，bridge 的 session `/execute/stream` 现在直接转发这些帧，不再先等待完整 JSON 结果。
