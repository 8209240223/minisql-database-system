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
- 覆盖 NDJSON 元数据/行/完成帧、结果顺序、写语句前置拒绝、响应断开后的后续查询和背压写入路径。
- `build-executor.ps1`：15 项执行器流式算子与预算提前停止检查通过。

## 未完成

- `SeqScan`/`IndexScan` 数据源仍基于 HeapStore 的 push 式 `scan` 一次性物化，尚未提供 pull 式存储迭代器，因此“扫描输入直接写 run”的端到端无界流式仍未打通；当前流式收益集中在 Filter/Project/Sort/Limit/Distinct 算子与预算提前停止。
- 尚未实现流式 HashJoin/HashAggregate、磁盘空间不足模拟、超大单分组和完整 X25 资源预算压力验收。
- 上述 database.cpp 集成依赖完整 `minisql_database.exe` 构建后回归，尚未进行集成验证。
