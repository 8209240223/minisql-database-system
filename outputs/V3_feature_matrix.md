# MiniSQL V3 实际功能矩阵

审计日期：2026-09-13

源码基线：`6839d8d` + 本轮 P0/P1 实现分支

需求基线：`MiniSQL数据库管理系统_完整详细需求规格说明书_V3.md`

## 1. 判定口径

本文件按当前源码、CMake 目标、启动脚本、前端调用链和测试入口重新审计，不沿用旧完成报告中的自述结论。

| 状态 | 含义 |
| --- | --- |
| 已实现 | 当前源码存在完整调用链，并有对应自动化测试证据 |
| 部分实现 | 主路径可用，但需求中的接口字段、组合场景、资源边界或验收条件尚未闭合 |
| 未实现 | 源码明确返回 `NotImplemented`、501，或只有固定占位结果 |
| 结构占位 | 构建目标存在，但没有该模块的实际实现代码 |

“存在文件、接口、按钮或测试脚本”本身不作为已实现证据。压力测试、浏览器测试或 CTest 没有被默认验收脚本执行时，也不写成“全量通过”。

## 2. 当前项目实际结构

| 层次/入口 | 实际位置 | 当前作用 | 判定 |
| --- | --- | --- | --- |
| 公共层 | `outputs/minisql-backend/src/common`、`src/security/access_catalog.cpp` | 错误、配置、日志及访问目录 | 已实现 |
| SQL 前端 | `src/sql/lexer.cpp`、`parser.cpp` | Tokenize、Parser、AST | 已实现；Token 包含 UTF-8 半开字节区间和起止行列 |
| Binder | `src/sql/binder.cpp` | 名称、作用域和访问对象绑定 | 已实现；相关引用使用可链式作用域身份 |
| Catalog | `src/catalog/catalog.cpp`、`persistent_catalog.cpp` | 内存视图及页式持久目录 | 已实现；v6 Catalog 使用稳定数值 `typeId` 并兼容旧描述符 |
| Planner | `src/sql/planner.cpp` | Logical Plan、子查询、派生表和聚合降级 | 已实现；派生表 JOIN 及受限可更新派生表 DML 已接通 |
| Optimizer | `src/optimizer/optimizer.cpp` | 固定点规则、下推、通用必要列传播和 Join 代价选择 | 已实现需求内主路径；EXPLAIN ANALYZE 节点含 inclusive-subtree I/O |
| Execution | `src/execution/database.cpp`、`executor.cpp` | DDL/DML/查询、事务、流式结果、EXPLAIN | 已实现；流式结果执行和传输均有 100000 行硬上限 |
| Storage | `src/storage` | PageFile、Buffer Pool、Heap、B+ 树、WAL/检查点 | 已实现 |
| 事务实现 | `Database` / `PageFile` / WAL | 执行层与存储层共同实现事务恢复 | 已实现；已删除无代码的 `minisql_transaction` 占位目标 |
| `minisql_lexer` | `src/server/lexer_main.cpp` | 独立词法入口 | 已实现 |
| `minisql_compile` | `src/server/compile_main.cpp` | 独立编译入口 | 已实现 |
| `minisql_database` | `src/server/database_main.cpp` | 真正的数据库会话/帧协议进程 | 已实现，是 Node bridge 调用的核心入口 |
| `minisql` | `src/main.cpp`、`src/server/application.cpp` | 数据库 CLI | 已实现；`-e/--execute` 和交互模式共用 `execution::Database` |
| 当前 HTTP bridge | `scripts/database-bridge.mjs` | HTTP、会话、权限、备份、NDJSON 适配 | 已实现，依赖 `minisql_database` 子进程；不是 C++ 原生 HTTP 服务 |
| 兼容 HTTP bridge | `scripts/bridge.mjs` | 旧入口兼容层 | 已实现；明确弃用并转发到真实 `database-bridge.mjs` |
| React 工作台 | `outputs/minisql-workbench/src` | 连接、标签级会话、编辑、结果、计划、索引维护、权限和存储界面 | 已实现主流程；完整可访问性证据仍归 UI09 |
| 验收脚本 | `outputs/run-minisql-tests.ps1` | Node 分组测试和前端单测/构建 | 部分覆盖，不等于仓库全量测试 |

## 3. 核心契约矩阵

| 编号 | 状态 | 已实现证据 | 尚未实现/未闭合 |
| --- | --- | --- | --- |
| REQ-CORE-001 输入资源与数值边界 | 已实现 | 输入、批量、标识符、诊断数、整数范围、Token UTF-8 半开范围及 256/257 层嵌套边界均有回归 | 未发现需求内明确缺口 |
| REQ-CORE-002 稳定接口契约 | 已实现 | Catalog 指纹、`PLAN_STALE_SCHEMA`、AST/Plan `nodeId/sourceSpan/outputSchema` 和 Token 起止范围均有往返/唯一性 contract | 未发现需求内明确缺口 |
| REQ-CORE-003 行页布局与生命周期 | 已实现 | Page、Heap、RowId/slot generation、空闲页和损坏检查存在 C++ contract | 未发现需求级明确空实现 |
| REQ-CORE-004 缓冲区和统计口径 | 已实现 | Buffer Pool、LRU/FIFO、pin/dirty、I/O 统计和 contract 存在 | 工作台固定序列实验属于 UI08 边界，不影响后端主路径判定 |
| REQ-CORE-005 持久化与提交边界 | 已实现 | 页文件、WAL、checkpoint、错误状态和重启恢复路径存在 | 高并发/故障组合仍由 X22/X26 单列 |
| REQ-CORE-006 固定端到端验收 | 已实现 | `minisql.exe`、`minisql_database.exe` 和 bridge 均调用真实数据库核心；CLI process smoke 验证 DDL/DML/查询 | 未发现需求级明确空实现 |

## 4. X01-X27 扩展功能矩阵

| 编号 | 功能 | 状态 | 实际实现与缺口 |
| --- | --- | --- | --- |
| X01 | 算术与复杂表达式 | 已实现 | 四则、一元运算、优先级、溢出/除零、短路及优化前后一致性均有实现和测试 |
| X02 | NULL 与三值逻辑 | 已实现 | NULL 位图、比较、布尔逻辑、排序、去重、外连接和聚合路径已接通 |
| X03 | 类型与 CAST | 已实现 | BIGINT/DECIMAL/FLOAT/BOOL/DATE/VARCHAR 与 CAST 主路径可用；Catalog v6 使用稳定数值 `typeId`、参数对象、旧格式兼容和未知 ID 拒绝 |
| X04 | UPDATE | 已实现 | 多赋值旧行求值、变长迁移、约束、索引、事务和重启路径存在 |
| X05 | ORDER BY 与分页 | 已实现 | 多键、别名、NULL 顺序、LIMIT/OFFSET 和外部排序已实现 |
| X06 | GROUP BY/聚合/HAVING | 已实现 | 五类聚合、NULL、HAVING 和外部分组/归并均有实际执行路径 |
| X07 | DISTINCT | 已实现 | 表达式、NULL、排序分页和持久化场景已覆盖 |
| X08 | JOIN | 已实现 | INNER/LEFT/RIGHT/FULL、自连接、直接等值 HashJoin 及派生表输入可执行；内存不足时采用分区外存 Hash Join，并清理临时分区 |
| X09 | 子查询 | 已实现 | IN/NOT IN/EXISTS/标量、派生表、多层相关作用域和祖先参数缓存均可用；三层父级+祖父级及仅祖父级引用通过实际执行 |
| X10 | 投影与别名 | 已实现 | 表达式/表别名/列别名/限定星号、嵌套派生表均可用；计算列拥有稳定 `expressionId` 并支持计划往返 |
| X11 | 约束与多行 VALUES | 已实现 | NOT NULL、DEFAULT、主键、复合键、UNIQUE、CHECK、外键以及语句级原子性已实现 |
| X12 | 诊断与纠错 | 已实现 | 多诊断、恢复、源码片段、建议及写操作不执行路径已接通 |
| X13 | 可扩展 AST/LR | 已实现 | AST/Plan/Catalog 版本拒绝与迁移入口、LR(0)/LR(1)/LALR 表生成器存在 |
| X14 | AST/Plan JSON | 已实现 | 版本包装、往返、损坏拒绝、稳定唯一 `nodeId`、`sourceSpan`、`outputSchema` 和 children 契约均通过 |
| X15 | 优化规则框架 | 已实现 | 固定点、预算、禁用规则、收敛诊断及规则记录存在 |
| X16 | 列裁剪与冗余节点 | 已实现 | 必要 binding 可穿过 Aggregate/Sort/Filter；JOIN 保留连接键和物理行边界，DML 保留 RowId/约束列，优化前后结果 contract 通过 |
| X17 | 谓词下推与连接改写 | 已实现 | INNER JOIN 单表谓词下推、外连接安全边界和等值 HashJoin 改写已有规则及 contract |
| X18 | 统计信息与代价 | 已实现 | 表列/索引统计、直方图、实时表行数和 Scan 成本可用；Join 在 NestedLoop/Hash 两候选间按唯一键估计与内存预算选择，`optimizerDecision` 可追踪 |
| X19 | EXPLAIN/ANALYZE | 已实现 | 原始/优化计划、实际行数、耗时、写语句不执行和逐节点 inclusive-subtree I/O 已实现 |
| X20 | B+ 树与索引 | 已实现 | 页级树、分裂、借位/合并、范围查询、删除、恢复及 Inspect 接口存在 |
| X21 | 事务与原子性 | 已实现 | BEGIN/COMMIT/ROLLBACK、SAVEPOINT、失败回滚和会话事务可用；实现位于执行/存储层，空 transaction 目标已删除 |
| X22 | WAL/检查点/故障注入 | 已实现 | WAL、重做、自动/后台 checkpoint、LSN 和故障注入路径存在 |
| X23 | 并发控制 | 已实现 | 多会话、数据库级两阶段锁、等待/超时、关闭回滚存在；当前声明方案不是 MVCC/行级锁 |
| X24 | 权限与审计 | 已实现 | 用户、角色继承、授权、密码、审计、热重载及 HTTP/CLI 身份链路存在；全部已支持语句统一经 AST/Catalog 绑定，`DROP INDEX` 可反查所属表并 fail-closed |
| X25 | 外部执行、取消与资源 | 已实现 | 外部排序/聚合、资源管理、取消、NDJSON 顺序背压可用；C++ 和浏览器各自限制 100000 行并报告截断，断流/超时取消不会击穿 bridge |
| X26 | 备份恢复与迁移 | 已实现 | 全量/增量、版本迁移、校验、失败恢复目录和 Windows UTF-8 在线快照路径均可用；在线备份 22 项通过 |
| X27 | Fuzz 与长期回归 | 部分实现 | 固定种子、差分、状态机、崩溃恢复、压力、自动 delta-debugging 最小化和重放入口存在；长时 soak 仍未形成默认持续验收证据 |

汇总：26 项已实现，1 项部分实现，0 项整项完全空白。剩余部分实现仅为 X27 长时 soak 证据，不再包含已识别的 P0/P1/P2 功能缺口。

## 5. UI01-UI10 工作台矩阵

| 编号 | 功能 | 状态 | 实际实现与缺口 |
| --- | --- | --- | --- |
| UI01 | 连接与能力发现 | 已实现 | 真实连接、状态、鉴权和能力接口可用；每个 QueryTab 保存无密码连接快照和独立后端 session 归属 |
| UI02 | 对象浏览器 | 已实现 | 真实表、列、类型、索引、刷新、过滤和生成查询入口存在 |
| UI03 | SQL 编辑与多标签 | 已实现 | 每标签独立保存连接、session、事务、结果、诊断、编辑版本和运行状态；可并行执行且乱序响应按 tab ID 写回，关闭标签释放其 session |
| UI04 | 编译、执行与取消 | 已实现 | 编译/执行分离、requestId、取消、超时、危险 UPDATE/DELETE 确认路径存在 |
| UI05 | 查询结果与反馈 | 已实现 | 类型化列、NULL、CSV、虚拟窗口可用；流式客户端使用 100000 行有界缓存并显示后端/客户端截断状态 |
| UI06 | 编译中间结果 | 已实现 | Token/AST/Plan/诊断视图展示稳定 Token 起止范围、AST/Plan 节点身份、源码范围和输出 Schema |
| UI07 | 诊断与源码定位 | 已实现 | 多诊断、行列定位、Unicode/选区偏移和网络错误区分已有实现与测试 |
| UI08 | 历史、设置与存储统计 | 已实现 | 历史/设置持久化、检索、存储统计及真实后端来源存在 |
| UI09 | 布局与可访问性 | 部分实现 | 响应式布局和若干浏览器断言存在；未形成键盘完成全部常用操作、200% 缩放和三视口完整可访问性证据 |
| UI10 | HTTP 集成契约 | 已实现 | UTF-8、错误状态、超限、进程失败、501、会话和请求体限制有协议级测试 |

汇总：9 项已实现，1 项部分实现（UI09 可访问性完整验收）。

## 6. P0/P1 完成状态与剩余缺口

本轮原清单中的 3 项 P0 和 6 项 P1 均已实现并进入回归：

| 原优先级 | 原缺口 | 当前状态 | 验证证据 |
| --- | --- | --- | --- |
| P0 | 在线备份失败 | 已完成 | Windows 中文路径使用 UTF-8 转换；`backup-online-smoke.mjs` 22 项通过 |
| P0 | 流式查询无端到端结果上限 | 已完成 | C++ 与客户端均有 100000 行硬上限/截断状态；session stream 16 项和浏览器取消回归通过 |
| P0 | `minisql.exe` 是 SQL 外壳 | 已完成 | `-e/--execute`、交互模式接入真实 `execution::Database`；`cli.process_smoke` 通过真实 DDL/DML/SELECT |
| P1 | 派生表 JOIN/UPDATE/DELETE | 已完成 | 支持派生 JOIN 左输入及受限可更新派生表；不可更新形状返回 2003；database contract 覆盖成功与拒绝边界 |
| P1 | 多层相关子查询 | 已完成 | 执行器使用祖先参数作用域栈，缓存键包含祖先绑定；三层与仅祖父级引用均通过 |
| P1 | AST/Plan/Token 稳定传输契约 | 已完成 | Token UTF-8 半开字节范围；AST/Plan `nodeId/sourceSpan/outputSchema` 唯一性、children 和往返 contract 通过 |
| P1 | 通用列裁剪和 Join 代价选择 | 已完成 | 必要 binding 穿过 Aggregate/Sort/Filter，JOIN/DML 保留隐藏物理列；两类 Join 候选、成本、内存预算和理由进入 `optimizerDecision` |
| P1 | 标签不是独立会话单元 | 已完成 | 单元测试与浏览器回归验证独立 session、并行执行、乱序响应隔离、关闭释放和无敏感持久化 |
| P1 | 索引维护 UI 不完整 | 已完成 | Verify/Rebuild 入口、确认、忙状态和结果提示已接入；浏览器回归验证两个真实请求 |

本轮八项 P2 已闭合；剩余仅为长期验收项：

| 优先级 | 缺口 | 直接证据 | 完成标准 |
| --- | --- | --- | --- |
| 长期验收 | X27 长时 fuzz/soak | 短回归和自动最小化已具备 | 在 CI/专用环境持续运行小时级/天级 soak 并归档趋势 |
| 长期验收 | 多小时 soak 证据需持续积累 | 默认短回归不形成跨版本资源趋势 | 建立独立长时 CI 阶段并保存资源趋势和最小失败样本 |

## 7. 验收脚本覆盖缺口

`powershell -ExecutionPolicy Bypass -File ./outputs/run-minisql-tests.ps1 -Suite all` 当前只运行选定的 Node 测试、前端若干单测和 `npm run build`。它没有执行：

- CTest/C++ contract 测试；
- `tests/cast-process.mjs`；
- `tests/backup-online-smoke.mjs`；
- fuzz 差分、状态机、崩溃恢复、pressure、long-run、soak 测试；
- 前端 `test:browser` 浏览器回归。

本轮已手动补跑 CTest、在线备份、流式进程、前端标签单测、生产构建和浏览器回归；但默认 `-Suite all` 仍没有自动纳入这些入口。建议新增 `full` 套件，明确区分短回归、浏览器回归、故障注入和长时测试，并让 CI 报告每个实际执行的命令。

## 8. 与规格书的行为偏离

| 条款 | 规格要求 | 当前行为 | 判定 |
| --- | --- | --- | --- |
| `==` 运算符 | 默认要求拒绝并返回 `LEX_UNSUPPORTED_OPERATOR` | Lexer 接受，Parser 规范化为 `=` | 实现超集，但不符合当前默认验收口径 |
| RIGHT/FULL JOIN | 精确语法清单外应明确拒绝 | 当前已经实现 | 实现超集，不应作为 X08 必需完成证据 |

## 9. 本轮验证说明

- Release 原生目标完整构建成功；仅保留已有的 `date.hpp` 名称遮蔽编译警告。
- `ctest --test-dir build/verification -C Release`：20/20 通过；其中 CLI process smoke 已改为验证真实 DDL/DML/SELECT。
- `minisql_optimizer_contract`：525 项通过；`minisql_database_contract`：89 项通过；Planner AST/Plan/Token contract 通过。
- `backup-online-smoke.mjs`：22 项通过；`session-stream-process.mjs`：16 项通过。
- `nesting-depth-process.mjs`：57 项通过，覆盖 256 层接受、257 层拒绝和 2000 层不崩溃。
- `npm run test:tabs`：2 项通过；`npm run build` 通过。
- 浏览器已验证编译视图、标签独立会话/乱序响应、索引 Verify/Rebuild，以及成功、语义失败、超时、活动取消、备份恢复、失败隔离与恢复后查询。
- 长时 fuzz/soak 与多小时资源趋势未在本轮执行，不写成通过。

## 10. 结论

本轮列出的 P0/P1/P2 功能项已全部实现。项目仍不能宣称“所有长期验收完成”：UI09 完整可访问性证据以及长时 fuzz/soak 仍待持续环境验证。
