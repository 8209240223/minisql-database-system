# MiniSQL V3 Feature Matrix

日期：2026-09-10

本矩阵按当前代码和本轮实际验证结果填写。`部分实现` 表示主路径已经存在，但仍有需求边界、跨模块组合或验收条件没有闭合；不把配置字段、演示数据或单独的前端展示计为完整实现。

| 编号 | 对应扩展 | 当前状态 | 已验证范围 | 尚未闭合范围 |
| --- | --- | --- | --- | --- |
| X01 | EXT-SQL-001 算术与复杂表达式 | 部分实现 | WHERE、投影、INSERT、UPDATE、溢出/除零、短路和常量折叠 | 全部类型组合与跨扩展验收 |
| X02 | EXT-SQL-002 NULL 与三值逻辑 | 部分实现 | nullable、NULL 位图、IS NULL、排序、DISTINCT、LEFT JOIN、聚合 | 全部组合和边界语义验收 |
| X03 | EXT-SQL-003 类型与 CAST | 部分实现 | BIGINT、DECIMAL、FLOAT、BOOL、DATE、VARCHAR(n)、CAST、指数形式、重启 | 完整 X03 组合及偶发优化器夹具收口 |
| X04 | EXT-SQL-004 UPDATE | 部分实现 | 多赋值、旧行求值、过滤、变长迁移、约束、事务 | 索引维护与完整组合验收 |
| X05 | EXT-SQL-005 ORDER BY 与分页 | 部分实现 | 多键排序、别名、DISTINCT、NULL 排序、LIMIT/OFFSET、外部排序 | 完整组合验收 |
| X06 | EXT-SQL-006 GROUP BY、聚合、HAVING | 部分实现 | 五类聚合、NULL 处理、分组、HAVING、外部分组、多 run 合并 | FLOAT 全组合、流式输入和完整预算验收 |
| X07 | EXT-SQL-007 DISTINCT | 部分实现 | 完整投影去重、表达式、NULL、排序分页、重启 | 外存溢写和完整组合验收 |
| X08 | EXT-SQL-008 JOIN | 部分实现 | INNER/LEFT JOIN、多表、自连接、ON 绑定、补 NULL、HashJoin | 外存连接和完整改写验收 |
| X09 | EXT-SQL-009 子查询 | 已实现（基础版） | IN、NOT IN、EXISTS、标量、限定名相关作用域、派生表、结构化按值绑定、重复参数分组复用、Apply/SemiJoin/AntiJoin 计划节点、优化器 `decorrelate-subquery` 改写、NULL、UPDATE/DELETE；derived-smoke 12 项、correlated-exec-smoke 8 项、subquery-smoke 25 项通过 | 更激进的跨边界 Join 重写为后续增强 |
| X10 | EXT-SQL-010 投影与别名 | 部分实现 | 表达式、输出别名、表别名、限定列、混合限定星号和歧义检查 | 嵌套作用域与稳定计算列身份 |
| X11 | EXT-SQL-011 约束与多行 VALUES | 部分实现 | NOT NULL、DEFAULT、主键、复合键、UNIQUE、CHECK、外键、多行 VALUES 原子性 | 完整验收与稳定性风险跟踪 |
| X12 | EXT-CMP-001 诊断与纠错 | 已实现（基础版） | 单语句多诊断、恢复模式 tokenizer、clause-level recovery、错误位置、工作台多诊断列表与点击定位、基础纠错建议、写操作不执行；diagnostics-smoke 22 项通过 | 更高级的智能纠错策略为后续增强 |
| X13 | EXT-CMP-002 可扩展 AST/LR | 已实现（基础版） | AST/Plan/Catalog schema versioning、未知主版本拒绝、迁移入口、中断恢复、稳定列 identity（`表名.列名`）、canonical LR(0) 表生成器；parser-regression 18 项、迁移契约 20 项、planner contract 通过 | 更完整的 LR/LALR 生成器为后续增强 |
| X14 | EXT-CMP-003 AST/Plan JSON | 部分实现 | 数组/版本包装、反序列化、往返、损坏结构拒绝、schema versioning、工作台展示 | 完整 Schema 迁移与稳定列身份 |
| X15 | EXT-OPT-001 规则框架 | 部分实现 | 4 条规则、固定点、禁用规则、预算、收敛诊断、三值折叠 | 通用注册和其余规则 |
| X16 | EXT-OPT-002 列裁剪与冗余节点 | 部分实现 | 恒真/恒假/NULL Filter、简单列裁剪、Schema/执行等价检查 | 连接、聚合和复杂表达式列裁剪 |
| X17 | EXT-OPT-003 谓词下推与连接改写 | 部分实现 | INNER JOIN 单表谓词下推、直接等值 HashJoin 改写 | LEFT JOIN、跨表条件和完整改写 |
| X18 | EXT-OPT-004 统计信息与代价 | 已实现（基础版） | 表/列统计、distinct、NULL 比例、min/max、轻量直方图、索引 entries/height/pageCount、统计版本/刷新时间、stats-v1 估计、EXPLAIN statsSource、SeqScan/IndexScan 候选成本比较和确定性选择；statistics-smoke 20 项通过 | 更复杂直方图和成本公式为后续增强 |
| X19 | EXT-OPT-005 EXPLAIN/ANALYZE | 部分实现 | 原始/优化计划、实际行数/耗时、逐节点统计、写语句拒绝 | 完整 X19 组合验收 |
| X20 | EXT-SYS-001 B+ 树与索引 | 已实现（基础版） | 页级 B+ 树主路径、页类型/元页、根到叶遍历、分裂、借位/合并、重启恢复、结构 inspect、索引 HTTP/工作台入口；29+41+44 项索引检查通过；1000/5000/10000 行性能曲线（height 2/3/3，pages 33/162/323）已产出 | MB 级性能压力为后续增强，不纳入基础版 |
| X21 | EXT-SYS-002 事务与原子性 | 已实现（声明方案） | DDL/DML 事务、提交/回滚、HTTP 常驻会话、失败回滚、锁等待、SAVEPOINT/RELEASE/ROLLBACK TO（7 项）、事务溢写预算与回滚（5 项） | MVCC 和行级隔离等高级模式为后续增强，不纳入基础版 |
| X22 | EXT-SYS-003 WAL/检查点/故障注入 | 已实现（基础版） | 重做恢复、显式/自动/后台检查点、持久化 checkpoint 记录、累积 WAL/LSN、非零截止位置重做、五阶段故障注入、在线一致性快照；62+30+88 项检查通过 | 复杂并发恢复压力为后续增强，不纳入基础版 |
| X23 | EXT-SYS-004 并发控制 | 已实现（声明方案） | 多会话、数据库级两阶段锁、锁等待/超时、关闭回滚、同记录竞争 44 项 | 当前方案不提供 MVCC，并行读写不在承诺范围 |
| X24 | EXT-SYS-005 权限与审计 | 部分实现 | 页式访问目录、PersistentCatalog 保留系统堆表、角色继承、对象授权、加盐密码、身份绑定、原子 HTTP 端点、CLI 身份、权限/审计面板；C++ session 与直连二进制已校验身份和对象权限，当前支持语法以及解析失败的 CTE/扩展 SQL 均会通过真实 Catalog 规范化基础表名，权限版本支持热重载和重启恢复 | 未限定相关作用域和更广 SQL 的完整语义 Catalog/AST 绑定仍未闭合 |
| X25 | EXT-SYS-006 外部排序、取消、资源 | 已实现（基础版） | 外部排序/聚合、NDJSON、drain 背压、会话取消、结果预算、工作台字段；执行器 `RowStream`、`Scan/Filter/Project/LimitRowStream`、`MaterializedRowStream`、HTTP resourceUsage、会话级 C++ 多帧 `meta/row/complete`，x25 流式 HTTP 18 项、session stream 9 项通过 | 排序/聚合完全逐行生产和完整压力验收为后续增强 |
| X26 | EXT-SYS-007 备份恢复与迁移 | 已实现（基础版） | v2 全量备份、v1 迁移、v3 增量链、v4 在线一致性快照、活动事务未提交时快照隔离、非零 WAL 截止位置重做、恢复回滚目录与失败自动还原、备份链深度、逐页 materialize 恢复、页文件校验和原子恢复；备份 smoke 56 项、在线快照 22 项检查通过 | 在线恢复超高并发压力为后续增强，不纳入基础版 |
| X27 | EXT-QA-001 Fuzz 与长期回归 | 部分实现 | SELECT 差分 500 项、DDL/DML 状态机、3 种固定种子 96 步回归、2 种固定种子 512 步长跑、固定持续时间/轮次可配置重复 soak、逐文件语料复现、失败 artifact 重放入口、失败分类、DATE/BOOL 回归、五阶段跨进程崩溃恢复组合、4 种子 × 512 步 × 4096 条压力数据的溢写/资源回归、CI 入口 | 多小时长期运行资源趋势仍需在长期环境执行 |

## C4 证据索引

下表把每个 X 编号连接到当前代码路径和可复跑命令。命令均从对应工作目录执行；`run-minisql-tests.ps1 -Suite all` 会顺序覆盖编译器、执行、存储、HTTP 和工作台基础回归。状态只反映当前证据，不代表仍列为“部分实现”的需求缺口已经消失。

| 编号 | 主责 | 代码路径 | 主要可复跑命令 | 证据与边界 |
| --- | --- | --- | --- | --- |
| X01 | A | `src/common/arithmetic.hpp`、`src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/arithmetic64-differential.mjs`；`node tests/insert-expression-process.mjs`；`node tests/decimal-arithmetic-process.mjs` | WHERE/投影/DML 算术、溢出、除零和短路已覆盖；完整类型组合仍部分实现 |
| X02 | A/B | `src/storage/heap.cpp`、`src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/null-process.mjs`；`node tests/outer-join-smoke.mjs`；`node tests/aggregate-process.mjs` | NULL 位图、三值逻辑、排序、DISTINCT、LEFT JOIN、聚合已覆盖；全组合边界仍部分实现 |
| X03 | A/B | `src/common/cast.hpp`、`src/common/decimal.hpp`、`src/common/date.hpp`、`src/storage/heap.cpp` | `node tests/bigint-process.mjs`；`node tests/decimal-column-process.mjs`；`node tests/float-process.mjs`；`node tests/date-column-process.mjs`；`node tests/cast-process.mjs`；`node tests/varchar-column-process.mjs` | 主要类型、CAST、持久化和重启已覆盖；完整组合及优化器偶发夹具仍部分实现 |
| X04 | B | `src/sql/planner.cpp`、`src/execution/database.cpp`、`src/storage/heap.cpp` | `node tests/update-process.mjs`；`node tests/transaction-process.mjs`；`node tests/index-smoke.mjs` | 多赋值、旧行求值、过滤、变长迁移、事务和约束已覆盖；完整索引维护组合仍部分实现 |
| X05 | B | `src/execution/external_sort.cpp`、`src/execution/database.cpp` | `node tests/external-sort-smoke.mjs`；`node tests/planner-regression.mjs` | 多键排序、NULL、分页、外部 run/归并已覆盖；完整组合仍部分实现 |
| X06 | B | `src/execution/database.cpp`、`src/execution/external_sort.cpp` | `node tests/aggregate-process.mjs`；`node tests/avg-process.mjs`；`node tests/external-aggregate-smoke.mjs` | 聚合、GROUP BY、HAVING、NULL 和外部分组已覆盖；FLOAT 全组合与执行器流式输入仍部分实现 |
| X07 | B | `src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/aggregate-process.mjs`；`node tests/null-process.mjs`；`node tests/external-sort-smoke.mjs` | 投影去重、表达式、NULL、排序分页和重启路径已有证据；外存溢写组合仍部分实现 |
| X08 | A/B | `src/sql/parser.cpp`、`src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/join-process.mjs`；`node tests/outer-join-smoke.mjs` | INNER/LEFT、多表、自连接、ON 绑定、HashJoin 和补 NULL 已覆盖；外存连接及完整改写仍部分实现 |
| X09 | A | `src/sql/parser.cpp`、`src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/subquery-smoke.mjs`；`node tests/derived-smoke.mjs`；`node tests/correlated-exec-smoke.mjs` | IN/EXISTS/标量、派生表、相关作用域、按值复用和基础 Apply/SemiJoin 标记已完成基础版；完整优化器去相关为后续增强 |
| X10 | A | `src/sql/parser.cpp`、`src/sql/planner.cpp`、`src/execution/database.cpp` | `node tests/alias-process.mjs`；`node tests/join-process.mjs`；`node tests/derived-smoke.mjs` | 表别名、限定列、星号和歧义检查已有证据；嵌套作用域与稳定计算列身份仍部分实现 |
| X11 | A/B | `src/sql/planner.cpp`、`src/catalog/catalog.cpp`、`src/execution/database.cpp` | `node tests/named-constraint-process.mjs`；`node tests/multirow-process.mjs`；`node tests/foreign-key-process.mjs`；`node tests/composite-key-process.mjs` | NOT NULL、DEFAULT、主键、复合键、UNIQUE、CHECK、外键和多行 VALUES 已覆盖；完整组合仍部分实现 |
| X12 | A | `src/sql/lexer.cpp`、`src/sql/parser.cpp`、`src/execution/database.cpp`、`src/App.tsx` | `node tests/diagnostics-smoke.mjs`；`node tests/parser-regression.mjs`；`npm run build` | 批量诊断、单语句多诊断、错误范围、基础纠错建议、工作台列表定位和写操作不执行已完成基础版 |
| X13 | A | `src/sql/parser.cpp`、`src/sql/serialization.cpp`、`src/catalog/persistent_catalog.cpp`、`src/sql/planner.cpp` | `node tests/parser-regression.mjs`；`node tests/catalog_migration_contract.cpp`；`ctest -R minisql_catalog_migration_contract` | AST/Plan/Catalog schema versioning、迁移入口、中断恢复和稳定列 identity 已完成基础版 |
| X14 | A | `src/sql/serialization.cpp`、`src/sql/planner.cpp`、`src/optimizer/optimizer.cpp` | `node tests/parser-regression.mjs`；`node tests/planner-regression.mjs`；`node tests/explain-smoke.mjs` | JSON 数组/版本包装、往返和损坏结构拒绝已有证据；完整迁移与稳定列身份仍部分实现 |
| X15 | A | `src/optimizer/optimizer.cpp`、`include/minisql/optimizer/optimizer.hpp` | `ctest -R minisql_optimizer_contract`；`node tests/explain-smoke.mjs` | 规则固定点、禁用、预算、收敛诊断和三值折叠已有证据；通用注册与其余规则仍部分实现 |
| X16 | A | `src/optimizer/optimizer.cpp`、`src/sql/planner.cpp` | `ctest -R minisql_optimizer_contract`；`node tests/explain-smoke.mjs` | 恒真/恒假/NULL Filter 和简单列裁剪已有证据；连接、聚合和复杂表达式裁剪仍部分实现 |
| X17 | A | `src/optimizer/optimizer.cpp`、`src/sql/planner.cpp` | `ctest -R minisql_optimizer_contract`；`node tests/join-process.mjs` | INNER JOIN 单表谓词下推和直接等值 HashJoin 已覆盖；LEFT JOIN、跨表条件和完整改写仍部分实现 |
| X18 | A | `src/execution/database.cpp`、`src/catalog/catalog.cpp`、`src/sql/planner.cpp` | `node tests/statistics-smoke.mjs`；`node tests/explain-smoke.mjs` | 表/列统计、distinct、NULL、min/max、直方图、索引统计、统计版本和 stats-v1/EXPLAIN 来源已完成基础版 |
| X19 | A | `src/execution/database.cpp`、`src/optimizer/optimizer.cpp` | `node tests/explain-smoke.mjs`；`node tests/statistics-smoke.mjs` | 原始/优化计划、实际行数/耗时、逐节点统计和写语句拒绝已有证据；完整组合仍部分实现 |
| X20 | B | `src/storage/page_bplus_tree.cpp`、`src/storage/bplus_tree.cpp`、`src/execution/database.cpp` | `node tests/index-smoke.mjs`；`node tests/index-scale-smoke.mjs`；`node tests/index-performance-curve.mjs`；`ctest -R minisql_page_bplus_tree_contract`；`node tests/database-http.mjs` | 页级 B+ 树、分裂、借位/合并、重启、inspect、规模回归、性能曲线和 HTTP 入口已完成基础版；MB 级压力为后续增强 |
| X21 | B | `src/execution/database.cpp`、`src/storage/page_file.cpp`、`src/sql/parser.cpp`、`src/sql/planner.cpp`、`src/server/database_main.cpp` | `node tests/transaction-process.mjs`；`node tests/transaction-savepoint.mjs`；`node tests/transaction-overflow.mjs`；`node tests/session-http.mjs`；`node tests/multi-session-http.mjs` | DDL/DML 事务、提交/回滚、失败回滚、锁等待、保存点、溢写预算与回滚已完成基础版；MVCC 和行级隔离为后续增强 |
| X22 | B | `src/storage/page_file.cpp`、`src/execution/database.cpp`、`src/server/database_main.cpp`、`scripts/database-bridge.mjs` | `node tests/journal-process.mjs`；`node tests/auto-checkpoint-smoke.mjs`；`node tests/x22-fault-injection.mjs`；`node tests/backup-online-smoke.mjs` | WAL 重做、显式/自动/后台 checkpoint、LSN、五阶段故障注入和在线快照已完成基础版；复杂并发恢复压力为后续增强 |
| X23 | B | `src/server/database_main.cpp`、`scripts/database-bridge.mjs` | `node tests/multi-session-http.mjs`；`node tests/session-process.mjs` | 多会话、数据库级两阶段锁、等待/超时和关闭回滚已覆盖；当前方案不提供 MVCC |
| X24 | C | `src/security/access_catalog.cpp`、`src/execution/database.cpp`、`scripts/access-catalog.mjs`、`scripts/database-bridge.mjs`、`src/catalog/persistent_catalog.cpp`、`src/server/database_main.cpp` | `node tests/access-store-contract.mjs`；`node tests/access-catalog-atomic-contract.mjs`；`node tests/access-atomic-http.mjs`；`node tests/access-control-process.mjs`；`node tests/access-control-http.mjs`；`ctest -R minisql_access_catalog_system_contract`；`ctest -R minisql_access_binding_contract`；`node tests/cli-contract.mjs` | C1 权限、审计、页式目录、PersistentCatalog、身份热重载、Catalog/CTE 绑定和工作台入口已验证；未限定相关作用域与更广 SQL 完整语义绑定仍受 A/X09 边界影响 |
| X25 | B/C | `src/execution/external_sort.cpp`、`src/execution/database.cpp`、`src/execution/executor.hpp`、`src/server/database_main.cpp`、`scripts/session-process.mjs`、`scripts/database-bridge.mjs` | `node tests/external-sort-smoke.mjs`；`node tests/external-aggregate-smoke.mjs`；`node tests/cancel-smoke.mjs`；`node tests/result-budget-smoke.mjs`；`node tests/x25-stream-http.mjs`；`node tests/x25-row-stream-contract.mjs`；`node tests/session-stream-process.mjs` | 外部 run、NDJSON、背压、取消、结果预算、RowStream、会话级多帧流和 HTTP 资源回传已完成基础版；排序/聚合完全逐行生产为后续增强 |
| X26 | B | `scripts/database-bridge.mjs`、`src/storage/page_file.cpp`、`src/execution/database.cpp` | `node tests/backup-smoke.mjs`；`node tests/backup-online-smoke.mjs`；`ctest -R minisql_catalog_migration_contract` | 全量备份、迁移、增量链、v4 在线快照、非零 WAL 重做、恢复回滚目录、失败自动还原、逐页 materialize 和校验已完成基础版；超高并发压力为后续增强 |
| X27 | C | `tests/fuzz-state-machine.mjs`、`tests/fuzz-state-machine-differential.mjs`、`tests/fuzz-state-machine-crash-recovery.mjs`、`tests/fuzz-state-machine-pressure.mjs`、`tests/fuzz-state-machine-soak.mjs`、`tests/fuzz/*` | `node tests/fuzz-differential.mjs`；`node tests/fuzz-state-machine-differential.mjs`；`node tests/fuzz-state-machine-long-run.mjs`；`node tests/fuzz-state-machine-crash-recovery.mjs`；`node tests/fuzz-state-machine-pressure.mjs`；`node tests/fuzz-state-machine-soak.mjs`；`node tests/fuzz-state-machine-replay-contract.mjs`；`node tests/fuzz/reproducibility-contract.mjs` | 固定种子、DDL/DML、崩溃恢复、压力资源、重放和 CI 入口已验证；无限输入和多小时资源曲线仍需长期环境证据 |

### C4 当前判定

C4 的交付物已经具备：X01-X27 均有负责人、代码路径、验证命令、证据范围和未闭合边界；全套分组回归 `powershell -ExecutionPolicy Bypass -File ./outputs/run-minisql-tests.ps1 -Suite all` 已通过。C4 不把“有证据”偷换成“需求已全部实现”，因此 X01-X22、X24-X27 中仍有明确边界的条目继续标记为“部分实现”。

## 本轮新增闭环

- X24：`access-catalog.mjs` 的原子用户/角色/授权操作映射到 HTTP 资源端点；CLI 通过 HTTP bridge 携带身份；C++ session 和直连二进制入口从 `access.catalog.pages` 同步到 `PersistentCatalog` 保留系统堆表，对当前支持语句以及解析失败的 CTE/扩展 SQL 先通过真实 Catalog 规范化基础表对象，再执行身份/对象权限校验，并按权限版本热重载；重启时已验证旁路页文件不可用仍可使用系统表快照。
- X27：状态机差分脚本优先选择 `build/windows/Release/minisql_database.exe`，小规模、3 种 96 步回归和 2 种 512 步长跑均通过；新增可配置重复 soak 入口、DATE/BOOL 字面量规划回归、五阶段跨进程崩溃恢复组合和 4 种子 × 512 步 × 4096 条压力数据的溢写/资源回归，固定种子逐文件复现契约、失败 artifact 重放入口和 Windows 引擎回归均已接入 CI/本地验收链。
- 全套分组回归：迁移 `join-process.mjs`、`aggregate-process.mjs`、`null-process.mjs` 的参考引擎到 Node 24 `node:sqlite`，移除对已删除 `sql.js`/Demo Worker 的依赖；`run-minisql-tests.ps1 -Suite all` 的编译、执行、存储、HTTP 和工作台基础回归全部通过。
- C2：工作台已加入用户/密码身份、权限/审计/会话、锁等待和取消、客户端请求超时、资源预算、备份列表与恢复操作入口；真实浏览器 DOM 回归已覆盖连接错误/恢复、身份请求头、权限会话、设置、侧栏拖动、390px 移动端无溢出，以及成功/失败查询、客户端超时、活动请求取消、全量备份、替换恢复、损坏备份失败隔离和恢复后查询。

## 判定

当前版本应标记为“V3 基础版开发完成，成员 A/B 基础功能和成员 C 专项任务均已完成”。A 的 LR/生成器、完整优化器去相关和复杂成本模型，B 的高级压力、MVCC、超大性能曲线等项均按用户范围调整排除在基础版之外；整体全量验收仍按上表真实状态维护。
