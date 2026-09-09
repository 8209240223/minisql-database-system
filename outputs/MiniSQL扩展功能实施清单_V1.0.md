# MiniSQL 扩展功能实施清单

版本：V1.0
日期：2026-09-08
用途：将最终需求文档中的所有扩展拆分为可实现、可验证的功能项，并记录当前工程状态。

## 一、状态定义

| 状态 | 含义 |
| --- | --- |
| 已实现 | 源码、接口和针对性测试已经存在，并通过当前检查 |
| 部分实现 | 已有基础代码或接口，但仍缺关键行为、持久化或完整测试 |
| 待实现 | 需求已定义，当前代码尚未实现 |
| 依赖阻塞 | 需要前置模块完成后才能实现 |

## 二、当前基础能力

| 功能 | 当前状态 | 证据 |
| --- | --- | --- |
| CMake/vcpkg/GoogleTest | 部分实现 | 工程配置和测试目标存在；独立核心 GCC 严格构建已运行，近期增量的完整主工程 CMake/MSVC 构建待验证 |
| 配置、命令行、错误、日志 | 部分实现 | `src/common`、`src/server` 已有实现；独立 compile/database 入口可用，原 application 壳仍有 NotImplemented 分支 |
| Lexer | 已实现 | `src/sql/lexer.cpp`、Token 输出程序 |
| Parser/AST | 部分实现 | DDL/DML、事务、连接、聚合、参数化类型和非相关子查询已接通；相关子查询、派生表、完整源码范围和 Token 级诊断未完成 |
| Catalog/语义 | 部分实现 | 页式目录、类型检查、默认值、键与 CHECK/外键约束、编译副本隔离已接通；稳定对象身份及完整版本迁移待完成 |
| 逻辑计划 | 部分实现 | 扫描、过滤、投影、DML、连接、聚合、排序和分页可执行，已接四条优化规则；索引计划、完整版本校验等待完成 |
| Page/File | 部分实现 | 4096 字节槽式页、代次、校验、文件分配释放已连接 Buffer、目录和执行器；溢出页、索引等待完成 |
| Buffer Pool | 部分实现 | LRU/FIFO、PageGuard、脏页刷新已接入持久化执行链；完整 I/O 统计与扩展资源管理待完成 |
| 行存储/页式目录 | 部分实现 | INT/BIGINT/VARCHAR/DECIMAL/BOOL/DATE/VARCHAR(n) 已接 SQL、事务和 API；2683 项底层检查通过，格式迁移及溢出页待完成 |
| SQL 真实执行 | 部分实现 | 独立 C++ Database 入口消费计划并读写真实页，支持显式事务；HTTP 与前端已连接，完整 SQL 扩展待完成 |
| React 工作台 | 部分实现 | 真实 C++ 执行、编译展示、多标签、事务、错误定位和 UTF-8 文件操作已有浏览器检查；完整工作台验收未关闭 |
| C++ HTTP API | 部分实现 | Node bridge 调用真实 C++ 编译与持久化执行；SessionRegistry 多逻辑会话、数据库级排他事务锁、锁等待、取消和只读 NDJSON 流式执行已接入。执行器级流式迭代、权限 UI 和完整组合验收待完成 |

## 三、SQL 扩展

| 编号 | 功能 | 依赖 | 当前状态 | 验收编号 |
| --- | --- | --- | --- | --- |
| EXT-SQL-001 | 算术与复杂表达式 | Parser、类型系统、求值器 | 部分实现：WHERE、投影、INSERT 和 UPDATE 表达式已接通，含溢出/除零、短路及折叠；完整跨类型与跨扩展验收待完成 | X01 |
| EXT-SQL-002 | NULL 与三值逻辑 | 类型系统、行格式 | 部分实现：位图持久化、nullable、三值逻辑、IS NULL、排序、DISTINCT、LEFT JOIN 和分组聚合已接通；完整跨扩展验收待完成 | X02 |
| EXT-SQL-003 | BIGINT、DECIMAL、FLOAT、BOOL、DATE、VARCHAR(n)、CAST | 类型系统、序列化 | 部分实现：BIGINT、DECIMAL、FLOAT、BOOL、DATE、VARCHAR(n) 列与持久化、CAST 已接通。FLOAT 已覆盖指数形式、有限值、运算、比较、聚合、显式 CAST、UPDATE、排序和跨进程重启，20 项专项检查通过；VARCHAR(n) 按码点计长、超长拒绝，既有类型检查通过。完整 X03 验收和偶发优化器夹具根因仍待收口 | X03 |
| EXT-SQL-004 | UPDATE | 行定位、写入、约束、事务 | 部分实现：多赋值、旧行求值、过滤、变长迁移、约束及事务已接通；索引维护和完整组合验收待完成 | X04 |
| EXT-SQL-005 | ORDER BY、LIMIT、OFFSET | 表达式、比较器、Sort | 部分实现：多键排序、隐藏源列、别名、DISTINCT、分页、NULL 排序和外部排序已接通；外部排序专项检查通过，完整组合验收待完成 | X05 |
| EXT-SQL-006 | GROUP BY、聚合、HAVING | NULL、Aggregate | 部分实现：五种聚合及 GROUP BY/HAVING 已可执行；整数、DECIMAL 表达式与持久化列聚合已有真实结果检查，低内存阈值下的外部分组、多 run 归并和状态合并专项检查通过。FLOAT 完整组合、流式输入、完整资源预算和 X06 全量验收仍待完成，详见 aggregate-progress.md、external-aggregate-progress.md、decimal-progress.md 与 decimal-storage-progress.md | X06 |
| EXT-SQL-007 | DISTINCT | Project、哈希/排序 | 部分实现：完整投影行去重、表达式、NULL、排序分页及重启已有检查；外存溢写和完整组合验收待完成 | X07 |
| EXT-SQL-008 | INNER/LEFT JOIN | 多表作用域、NULL | 部分实现：INNER/LEFT JOIN、多表/自连接、ON 绑定、补 NULL 与歧义检查；哈希连接、外存及完整组合验证待完成 | X08 |
| EXT-SQL-009 | 子查询、IN、EXISTS、标量子查询 | 多层作用域、Apply | 部分实现：非相关 IN/NOT IN、EXISTS、标量子查询和限定名相关子查询已接通，覆盖 NULL、空结果、投影、UPDATE/DELETE、ANALYZE 和错误拒绝；派生表、未限定相关作用域和 Apply/SemiJoin 待完成 | X09 |
| EXT-SQL-010 | 投影表达式和别名 | 表达式、Schema | 部分实现：表达式、输出/表别名、限定列、INNER JOIN 后混合限定星号及歧义检查已接；嵌套作用域及稳定计算列身份待完成 | X10 |
| EXT-SQL-011 | NOT NULL、DEFAULT、PRIMARY KEY、UNIQUE、CHECK、外键、多行 VALUES | Catalog、约束、事务 | 已接入 NOT NULL、DEFAULT、单列/复合键、CHECK、具名约束及外键；新增多行 VALUES 单语句原子提交、批内键检查与自引用最终状态检查，111 项专项检查通过；完整验收及未关闭稳定性风险仍需跟进 | X11 |

## 四、编译器扩展

| 编号 | 功能 | 依赖 | 当前状态 | 验收编号 |
| --- | --- | --- | --- | --- |
| EXT-CMP-001 | 错误恢复、批量诊断、智能纠错 | Parser、Catalog | 部分实现：新增语句级批量诊断接口，可返回多语句语法/语义错误且不执行写操作；Token 级恢复、单语句多诊断和智能纠错待完成 | X12 |
| EXT-CMP-002 | 可扩展 AST、LR/生成器方案 | AST 接口、序列化 | 待实现 | X13 |
| EXT-CMP-003 | AST/Plan JSON、结构展示、版本迁移 | AST、Plan | 部分实现：AST 与 Plan 均支持数组/版本包装反序列化和序列化往返，损坏 ID、父链和表达式会拒绝；完整 Schema 迁移、稳定列身份和 X14 验收待完成 | X14 |

## 五、优化器扩展

| 编号 | 功能 | 依赖 | 当前状态 | 验收编号 |
| --- | --- | --- | --- | --- |
| EXT-OPT-001 | 规则框架、固定点、等价性验证 | Logical Plan | 部分实现：4 条规则、按 ID 禁用、固定点、节点/轮数预算、收敛诊断及类型/Schema 保护；新增 NULL 比较和完整三值常量折叠，501 项优化器检查与 HTTP 回归通过。通用注册及其余规则待完成，见 optimizer-progress.md | X15 |
| EXT-OPT-002 | 列裁剪、恒真/恒假和冗余节点消除 | Schema、Plan | 部分实现：恒真/恒假/NULL Filter 改写和简单 Project->Filter?->SeqScan 列裁剪已接通，投影/过滤引用列和字面量投影均有 Schema/执行等价检查；复杂连接、聚合和表达式列身份裁剪待完成 | X16 |
| EXT-OPT-003 | 谓词下推、连接改写 | JOIN、NULL | 部分实现：INNER JOIN 单侧、无 CAST/算术/子查询的安全谓词可下推；直接左右列等值连接改写为 HashJoin，LEFT JOIN、跨表条件和完整连接改写待完成 | X17 |
| EXT-OPT-004 | 统计信息与简单代价模型 | Catalog、Index | 部分实现：新增表/列统计接口，返回行数、页数、distinct、NULL 数和 NULL 比例；EXPLAIN 使用 stats-v1 选择率估计；完整列/索引统计、直方图和可比较代价模型待完成 | X18 |
| EXT-OPT-005 | EXPLAIN、EXPLAIN ANALYZE、执行统计 | Plan、Executor | 部分实现：EXPLAIN 不执行目标语句，只读 EXPLAIN ANALYZE 返回实际行数、耗时、缓存增量和逐节点实际行数/耗时；写语句拒绝和完整 X19 验收待收口 | X19 |

## 六、存储与数据库系统扩展

| 编号 | 功能 | 依赖 | 当前状态 | 验收编号 |
| --- | --- | --- | --- | --- |
| EXT-SYS-001 | B+ 树、复合/唯一索引、IndexScan | Page、RowId、Catalog | 部分实现：索引镜像已由 JSON sidecar 迁移到页文件 owner 页，Catalog 暴露 page-file/pageCount/height，支持根分裂、范围查询、删除、重启恢复和强制重建，44 项专项检查通过；页级节点遍历、索引页类型编号与页内借位/合并未完成，完整 X20 验收待收口 | X20 |
| EXT-SYS-002 | BEGIN、COMMIT、ROLLBACK、原子性 | WAL、锁或版本 | 部分实现：显式事务、事务化 DDL、常驻 HTTP 会话及工作台事务控制；进程/HTTP 测试和真实浏览器提交、回滚、失败、过期、响应丢失检查通过；多逻辑会话采用数据库级排他两阶段锁，锁等待、超时和关闭回滚已接通，行级隔离/MVCC 和 X21 全量组合验收未完成 | X21 |
| EXT-SYS-003 | WAL、恢复、检查点、故障注入 | Page、事务 | 部分实现：整页重做日志、同步、独占锁、身份校验、重复恢复、提交后截断、显式 CHECKPOINT，以及按写语句/WAL 累计大小/提交脏页数量/脏页比例/时间窗口触发的自动 CHECKPOINT 已接通；事务 COMMIT 统一评估，新增五阶段跨进程故障注入和 30 项检查；当前仍为提交事件驱动，后台调度、独立 WAL 截止位置语义和 X22 全量组合验收待完成，详见 `docs/auto-checkpoint-progress.md` | X22 |
| EXT-SYS-004 | 并发控制、锁、死锁、MVCC 替代方案 | Executor、事务 | 已实现：选择保守数据库级两阶段锁并固定为交付方案；SessionRegistry 多会话、事务锁等待/超时、关闭回滚和同记录 UPDATE 竞争 44 项检查通过，详见 concurrency-progress.md。非 MVCC，不并行读写，前端多会话面板未收口 | X23 |
| EXT-SYS-005 | 用户、角色、对象权限、审计 | Catalog、会话 | 部分实现：访问目录已支持用户/角色/角色继承/对象授权、加盐密码、跨会话身份、元数据过滤、审计 object 字段与过滤，37 项专项检查通过；新增 `createUser`/`dropUser`/`createRole`/`dropRole`/`setPassword`/`grant`/`revoke` 纯函数原子接口（27 项契约）并映射为独立 HTTP 资源端点（39 项集成，2026-09-09），工作台新增用户/角色/会话/审计面板。访问目录尚未进入页式 Catalog、CLI 强制身份校验待完成 | X24 |
| EXT-SYS-006 | 外部排序、大结果集、取消和资源预算 | Sort、Buffer | 部分实现：ORDER BY 超过内存行预算时写入带 sessionId、查询/排序序号和 FNV-1a 校验的 JSONL run 并多路归并，临时文件结束或异常清理，专项检查 13 项通过；分组输入超过预算时按分组键外部排序并合并状态，外部聚合专项检查 8 项通过；新增只读 NDJSON 流式接口和 TCP drain 背压路径，12 项 X25 流式检查通过。执行器级迭代、提前停止、完整资源预算和 X25 全量验收待完成，详见 `docs/streaming-progress.md` | X25 |
| EXT-SYS-007 | 备份恢复、格式版本和迁移 | 持久化、WAL | 部分实现：v2 全量备份、v1 迁移、校验恢复已接通；新增 v3 离线按页增量备份与多级链恢复，42 项专项检查通过；在线一致性快照、非零 WAL 重做、迁移失败回滚目录和完整 X26 验收待完成 | X26 |

## 七、测试扩展

| 编号 | 功能 | 依赖 | 当前状态 | 验收编号 |
| --- | --- | --- | --- | --- |
| EXT-QA-001 | 固定种子 SQL 生成、变异、最小化、回归 | Lexer、Parser、Executor | 部分实现：SELECT 固定种子生成、4 类变异、SQLite 差分、模型缩减、故障分类及报告；新增 DDL/DML 状态机生成器、固定种子语料持久化（`tests/fuzz/generate-corpus.mjs` + `manifest.json`）与 `.github/workflows/ci.yml` 可重复回归入口（2026-09-09）。依赖 C++ 引擎的差分、跨进程组合与崩溃恢复回归仍待引擎构建后启用 | X27 |

## 八、实施顺序

1. 完成基础 Parser/AST 接入、Catalog、语义分析和 Logical Plan。
2. 完成 Page、File Manager、Buffer Pool、Row 编码和持久化目录。
3. 完成 Executor，闭合 CREATE、INSERT、SELECT、DELETE 真实执行链路。
4. 按 EXT-SQL-001 至 EXT-SQL-011 扩展语言和类型，并同步更新 AST、计划和存储格式。
5. 实现 EXT-CMP、EXT-OPT，确保优化只作用于语义正确的计划。
6. 实现索引、事务、WAL、并发、权限、外部排序和备份恢复。
7. 实现 Fuzz、故障注入、跨扩展组合测试，更新 `feature-matrix` 和实践报告。

## 九、实现状态规则

扩展清单中的“已实现”必须同时满足：代码存在、接口可调用、成功路径通过、失败/边界路径通过、重启或并发语义已验证（适用时），并能由前端或 CLI 展示结果。只有配置字段、空目录、CMake INTERFACE 目标、伪造结果或未实现错误不能计为实现。

工作台诊断增量：选区错误映射、源码跳转、起始字符下划线、行边标记、悬停错误消息与 EOF 定位已接入；源码变更清除、恢复及跨标签隔离通过真实浏览器检查。29 项位置映射检查和前端构建通过，见 minisql-backend/docs/workbench-diagnostics-progress.md。此项不表示 EXT-CMP-001 的后端错误恢复和多诊断已实现，也不关闭其他扩展。

## 十、真实工作台接入增量（2026-09-08）

真实 C++ HTTP 服务已接入工作台。成功编译可查看 Token、AST、原始与优化计划及规则记录；编译不会将 SQL 临时建表写入正式目录。页面提供 Explain 入口、原始/优化计划切换和节点数量统计；页面不再用父节点编号计算计划缩进，也不再推测未返回的阶段为已通过。

本轮证据：HTTP 29 项断言、浏览器 DOM 9 项断言、词法语法 12 项、语义计划 25 项、跨进程持久化 7 项均通过；C++ 两个入口及前端构建通过。详细范围和限制见后端 `docs/http-workbench-progress.md`。

EXT-CMP-003 仍为部分完成：共享 Token/AST 输出已接入，AST 与 Plan JSON 反序列化和往返已补齐，但完整 Schema 迁移契约仍未完成。HTTP 请求串行化不计为 EXT-SYS-004 数据库并发控制完成。工作台完整验收和其余扩展继续保留未完成状态。
