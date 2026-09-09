# 常驻进程与 HTTP 事务会话

日期：2026-09-08。跨请求事务已接入，每个桥接服务只允许一个活动会话，不代表多会话隔离完整实现。

## 一、常驻进程

minisql_database 的 session 模式持有同一个 Database 实例，启动发送 protocolVersion=1 的 ready 消息。每行 JSON 请求包含非空字符串 id、operation，以及 execute/compile 所需的 sql 字符串；支持 execute、compile、catalog、close。响应回传 id、success、transactionState 和大整数编码标记。

execute 保留事务，close 回滚活动或失败事务后退出。输入结束或进程终止会丢弃未发布的暂存页。半条消息到达 EOF 时拒绝执行。请求最多 8 MiB，JSON 深度最多 16；超限完整行报错后可以继续处理下一行。

scripts/session-process.mjs 串行发送请求，最多排队 64 条；默认超时 30 秒，响应缓冲上限 32 MiB。通信层校验响应 ID 和 UTF-8/JSON，协议错误、超时和异常退出会停止会话，不自动重试写请求。传输失败不能证明 COMMIT 未发生，应按提交状态未知处理。

## 二、HTTP 路由

1. POST /api/sessions 创建会话，返回 sessionId 和空闲超时。
2. POST /api/sessions/{sessionId}/execute 执行 SQL，BEGIN 与 COMMIT 可分别位于不同请求。
3. POST /api/sessions/{sessionId}/compile 使用会话可见目录，只编译不执行。
4. GET /api/sessions/{sessionId}/catalog 获取会话目录。
5. POST /api/sessions/{sessionId}/close 关闭并回滚未提交修改。

会话 ID 由 randomUUID 生成，只用于所有权识别，不映射文件路径，也不是完整身份认证。不存在或过期会话返回 404。会话占有数据库期间，第二次创建会话和无会话旧接口访问返回 409，不复用其他会话的事务。

默认空闲超时 300000 毫秒，MINISQL_SESSION_IDLE_MS 可配置为 100 至 3600000 毫秒。超时排队关闭并回滚，不中断执行中的请求；旧超时回调通过代次标记失效。执行中的 HTTP 连接异常断开时停止会话并隔离桥接服务，提交结果按未知处理，不假装已确定回滚。

能力报告为 transactions=true、sessionTransactions=true、scriptTransactions=true、maxSessions=1。旧的无会话 execute 接口仍是一次性脚本，不能在多个旧接口请求间保留事务。

## 三、验证

session-process.mjs 51 项通过：真实 C++ 进程跨请求事务、ID、队列、数据库占有、关闭、强制结束、EOF、截断、超大帧与畸形消息。session-transport.mjs 13 项通过：模拟工作进程的分片响应、ID 不匹配、非法 JSON、响应超限、超时和异常退出；仅通信故障使用模拟进程。

session-http.mjs 27 项通过：跨 HTTP 请求提交/回滚、失败事务状态、目录可见性、占用拒绝、会话关闭、空闲回滚与重新读取持久化数据。原有 database-http.mjs 全链路回归通过。

## 四、未完成范围

工作台现已接入会话 ID 和事务按钮，DOM 验证见 workbench-transaction-progress.md。SessionRegistry 多逻辑会话、数据库级排他事务锁、锁等待超时和关闭回滚已接入，详见 concurrency-progress.md；事务级行锁/表锁、死锁等待图、完整权限 UI、连接恢复和 X21/X23 全量验收尚未完成。当前数据库级排他仍会串行化并发事务，并非行级隔离或 MVCC。此前偶发自引用外键失败仍未关闭。
