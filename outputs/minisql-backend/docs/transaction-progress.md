# 显式事务核心与单请求事务脚本

日期：2026-09-08。EXT-SYS-002 部分交付。跨 HTTP 请求会话已接入，见 session-progress.md；多会话并发控制仍待完成。

## 一、语法和状态

支持 BEGIN、COMMIT、ROLLBACK，可选 TRANSACTION 后缀，均要求分号。SQL 编译输出 Begin/Commit/Rollback 独立逻辑节点，不执行数据修改；编译模拟目录按语句推进，ROLLBACK 恢复相应的临时目录，编译不会发布表。

Database 实例的状态为 IDLE、ACTIVE、ABORTED；提交结果标记 committed，随后回到 IDLE。提交状态不确定时实例不可继续使用，对外返回 UNKNOWN。BEGIN 进入 ACTIVE，所有后续写语句使用同一页写集合；查询及编译可见自己的未提交数据和目录。COMMIT 才发布整批日志。ROLLBACK 恢复数据、缓存和目录。嵌套 BEGIN、无活动事务时 COMMIT/ROLLBACK 都报事务错误。

显式事务中执行错误（词法、语法、语义、约束、表达式或运行异常）会撤销暂存修改并进入 ABORTED；之后除 ROLLBACK 外拒绝执行语句。ROLLBACK 确认后回到 IDLE。故障跨过日志发布提交点时不能声称已撤销，而返回 commitState=unknown 并要求重新打开恢复。

CREATE TABLE 与 INSERT/UPDATE/DELETE 共用事务，事务内新表可立即访问，回滚后表名和目录恢复。当前没有索引模块，因此不声称已经验证索引事务一致性。

## 二、调用和结果边界

Database.execute 可在同一个实例的多次调用之间保留事务；每次结果包含 transactionState，每条执行结果包含 commitState。事务内语句暂为 pending，同一次调用中后续 COMMIT/ROLLBACK 会把前面相应结果更新为 committed/rolledBack。已返回给调用者的旧响应不会被反向修改，调用者必须以随后事务结束结果更新界面。

Database.executeScript 用于一次性 CLI 和现有 HTTP 桥。一个请求可以提交完整事务脚本；请求结束还有活动事务时撤销并返回明确错误，不隐式提交，也不把 pending 结果作为成功持久化结果。异常后的事务同样清理。Database 销毁会丢弃未发布的内存页集合，断开不提交。

旧的无会话 HTTP 接口采用 executeScript，不保留跨请求事务。新的 /api/sessions 路由采用常驻进程，可以分次 BEGIN、写入和 COMMIT，能力报告 transactions=true、maxSessions=1，详见 session-progress.md。桥接汇总影响行数排除 rolledBack 结果，错误提示只把 committed 结果算作未撤销的已成功语句。

## 三、验证

transaction-process.mjs 74 项通过：多语句提交、显式回滚、第二条语句失败、查询除零、编译错误、非法字符、缺少分号、嵌套事务、EOF 未提交、事务化 DDL、已提交语句不被后续事务失败撤销，以及只编译不持久化。

`transaction-savepoint.mjs` 7 项通过：DML 回滚到保存点、DDL 回滚到保存点、RELEASE 后提交、事务外 SAVEPOINT 拒绝，以及保存点回滚后索引重建。

`transaction-overflow.mjs` 5 项通过：通过 `MINISQL_MAX_BATCH_PAGES` 缩小写批页数预算，大事务超限时返回明确的 `Write batch page limit exceeded; rollback required`，自动回滚后数据库仍可查询。

database_contract 81 项通过：同实例跨调用保留事务、读自己的写入、ABORTED 状态限制、ROLLBACK 恢复、目录撤销、重复提交拒绝、实例销毁后未提交记录消失，以及原生异常在日志提交点前后的状态与恢复。

HTTP 全链路回归通过，包括完整事务脚本、未提交请求结束、回滚后影响行数和提示、多行插入、外键及 12 个串行处理的并发写请求。多行专项 111 项重新运行通过。本轮未运行浏览器交互测试，不把 HTTP 通过视为前端事务按钮已完成。

## 四、保存点实现

- `SAVEPOINT` 在活动事务内保存页写批次和 Catalog 快照。
- `ROLLBACK TO [SAVEPOINT]` 恢复页写批次、Catalog 和索引，使 DML 与 DDL 都可回滚到保存点。
- `RELEASE [SAVEPOINT]` 删除保存点；COMMIT、ROLLBACK 和事务开始会清理保存点集合。
- 保存点名称按大小写不敏感匹配。

## 五、事务溢写边界

- `MINISQL_MAX_BATCH_PAGES` 可配置单写批次的页数上限，默认 16384。
- 超过预算时事务返回明确错误并回滚，不产生部分修改；该入口同时用于自动化回归。

## 六、尚未完成

前端事务按钮与状态同步已接入，见 workbench-transaction-progress.md。多逻辑会话、数据库级排他事务锁、锁等待超时和关闭回滚已接入，详见 concurrency-progress.md；行级隔离、死锁等待图和 X21/X23 全量验收尚未完成。事务溢写目前是明确预算/回滚语义，尚未实现降级到外存继续执行。原生数据库锁仍独占整个打开实例，不等于已经实现行锁或 MVCC。此前偶发自引用外键测试失败仍是未关闭风险。
