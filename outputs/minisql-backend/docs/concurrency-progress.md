# MiniSQL 多会话并发控制进度

## 方案

V3 剩余技术文档 8.1 允许在两阶段锁与 MVCC 中固定一种。本项目选择保守数据库级两阶段锁（serialized two-phase database lock）：页文件仍由单一 C++ 引擎进程独占，HTTP bridge 用 SessionRegistry 管理多个逻辑会话，事务锁只归属一个会话；其他会话在事务提交、回滚、关闭或锁等待超时前不得发送下一条语句。

该方案可串行化，不会出现脏读、不可重复读或范围幻读，也避免同一页文件被多个进程打开导致格式损坏。它不是 MVCC，也不是表级并行读写；锁超时而非死锁检测是本方案的受控解除方式，因为单一事务锁不可能形成等待环。

## 本轮实现

- C++ session 协议支持每次请求携带 `sessionId` 与 `cancelFile`，`Database::setSessionContext` 在语句前切换会话身份和取消文件，外部排序/聚合临时文件身份按逻辑会话记录。
- `POST /api/sessions` 改为创建逻辑会话，默认 `MINISQL_MAX_SESSIONS=16`；会话拥有独立事务状态、取消文件、活动时间、锁等待状态。
- `GET /api/sessions` 返回会话列表、活动请求、锁等待、事务锁归属和超时配置。
- 会话请求先获取逻辑执行权，再进入引擎队列；事务保持 ACTIVE/ABORTED 时继续占用事务锁，其他会话进入等待队列。
- COMMIT、ROLLBACK、会话关闭和空闲超时都会释放事务锁并唤醒等待者。
- 锁等待超过 `MINISQL_TRANSACTION_LOCK_TIMEOUT_MS`（默认 30000ms）返回 409，等待中的会话保持 IDLE，不进入半执行事务。
- 无活动会话后自动关闭共享引擎；备份、恢复和无会话一次性请求在 `sessions.size == 0` 时才能执行。
- capabilities 暴露 `multiSession`、`sessionRegistry`、`maxSessions`、`concurrencyModel`、`transactionLock` 与 `transactionLockTimeoutMs`。

## 验证

`node tests/multi-session-http.mjs`：44 项检查通过，覆盖两个会话并存、事务锁等待、提交后可见、同记录两次顺序 UPDATE、锁等待超时 409、关闭自动回滚、会话上限、一次性请求拒绝、无会话后正常执行和 capabilities。

既有回归：

- `session-process.mjs` 51 项通过。
- `session-http.mjs` 28 项通过。
- `cancel-smoke.mjs` 20 项通过。
- `database-http.mjs` 全套通过。
- `observability-http.mjs` 42 项通过。

## 限制

- 当前是保守锁，事务期间不并行执行其他会话的读或写。
- 尚未实现 MVCC、行级锁、表级并行读写和等待图死锁检测；当前方案用锁等待超时控制。
- 工作台前端仍只打开一个会话，多会话状态面板未接入。
- `MINISQL_MAX_SESSIONS` 上限只约束 HTTP 逻辑会话，单引擎进程仍保持单写者语义。
