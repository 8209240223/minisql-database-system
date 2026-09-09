# MiniSQL V3 未完成功能三人分工详细设计任务书

版本：V1.0

编制日期：2026-09-09

适用基线：`outputs/MiniSQL数据库管理系统_完整详细需求规格说明书_V3.md`、`outputs/MiniSQL_V3_剩余功能详细实现与验证技术文档.md`、`outputs/MiniSQL扩展功能实施清单_V1.0.md` 和 `outputs/V3当前实现状态.md`

## 一、文档目的

本任务书把 MiniSQL V3 当前尚未完成或仅部分完成的工程项拆成可以独立开发、联调、验证和评审的任务，并固定三名成员的责任边界。任务分配按系统层次划分，避免同一模块多人并行修改造成接口漂移；所有跨组能力先冻结接口契约，再进入实现。

本任务书中的“完成”必须同时满足以下条件：

1. 代码已经合入 `main`，不是本地未提交分支或临时脚本。
2. 成功路径、失败路径和边界路径均有自动化测试。
3. 涉及持久化、事务、索引、权限、取消或跨进程行为的任务必须通过重启、并发或故障注入验证。
4. 对外接口必须同步更新 HTTP capabilities、前端类型或文档。
5. 相关 X 编号专项验收通过后，才能把实施清单状态从“部分实现”改为“已实现”。
6. 只有 X01-X27 全量组合验收通过，项目状态才能写成“V3 全部完成”。

## 二、人员与职责总览

| 角色 | 负责人 | 主要职责 | 主责模块 | 不负责 |
| --- | --- | --- | --- | --- |
| 成员 A | 待填写 | 编译前端、AST、语义、计划、优化器和编译诊断 | X12、X13、X09 剩余、X18 剩余 | 页式索引、WAL、备份恢复、权限持久化 |
| 成员 B | 待填写 | 存储、索引、事务、WAL、执行器资源和备份迁移 | X20、X22、X25 执行器部分、X26 | 编译前端、权限 UI、最终验收报告 |
| 成员 C | 待填写 | 权限、审计、工作台、Fuzz 和全量验收 | X24、工作台收口、X27、X01-X27 矩阵 | 修改 A/B 主责模块的核心实现，需通过接口提出需求 |

三名成员共同负责跨组接口评审、每周集成检查、回归测试和最终交付报告。任何模块的负责人变更必须在任务书和实施清单中同步记录。

## 三、当前基线与未完成范围

### 3.1 已形成可运行链路

当前工程已经具备 C++ SQL 编译与真实执行、页式存储、目录持久化、WAL 重做恢复、事务、B+ 树索引、权限审计、外部排序、外部聚合、传输层流式结果和 React 工作台。已通过的主要专项能力包括：

| 能力 | 当前证据 |
| --- | --- |
| SQL 编译与执行 | DDL、DML、JOIN、聚合、排序、分页、子查询、类型和约束测试通过 |
| WAL 与自动检查点 | 自动检查点 62 项、故障注入 30 项、WAL 恢复 88 项通过 |
| 索引 | 索引专项 29 + 41 + 44 项通过 |
| 多会话 | 多会话 HTTP 44 项通过 |
| 权限与审计 | 访问控制 HTTP 37 项通过 |
| 备份恢复 | 备份恢复 42 项通过 |
| 流式传输 | X25 传输层流式 HTTP 12 项通过 |
| 工作台 | 构建通过，安全、历史和 CSV 单元测试通过 |

### 3.2 仍需完成的范围

| 编号 | 模块 | 当前状态 | 主要缺口 | 主责 |
| --- | --- | --- | --- | --- |
| X09 | 子查询与作用域 | 部分实现 | 派生表、未限定名相关作用域、Apply/SemiJoin、完整关联优化 | 成员 A |
| X12 | 编译器错误恢复 | 部分实现 | Token 级恢复、单语句多诊断、智能纠错且不执行猜测语句 | 成员 A |
| X13 | AST/Plan 与 Schema 迁移 | 部分实现 | 可扩展 AST/LR 或生成器方案、完整 Schema 迁移契约 | 成员 A |
| X18 | 统计与成本模型 | 部分实现 | 完整列/索引统计、直方图、可比较成本模型、确定性选择 | 成员 A |
| X20 | B+ 树索引 | 部分实现 | 页级节点遍历、索引页类型编号、页内借位/合并 | 成员 B |
| X22 | WAL 与检查点 | 部分实现 | 后台调度、独立 WAL 截止位置、活动事务边界和全量验收 | 成员 B |
| X24 | 权限与审计 | 部分实现 | CLI/API 共用权限、页式 Catalog、逐项 GRANT/REVOKE、权限 UI | 成员 C |
| X25 | 外部执行资源 | 部分实现 | 执行器级迭代、提前停止、完整资源预算和压力验收 | 成员 B，前端由成员 C 配合 |
| X26 | 备份与迁移 | 部分实现 | 在线一致性快照、非零 WAL 重做、迁移失败回滚目录 | 成员 B |
| X27 | Fuzz 与回归 | 部分实现 | DDL/DML 状态机、完整数据变异、CI 和长期回归 | 成员 C |
| 工作台 | UI 收口 | 部分实现 | 多会话、锁等待、取消、权限、审计、备份和资源错误展示 | 成员 C |
| 全量验收 | X01-X27 | 未完成 | feature matrix、HTTP、前端、跨进程和组合回归证据 | 成员 C 统筹，A/B 提供模块证据 |

## 四、总体验收规则

### 4.1 状态定义

| 状态 | 判定 |
| --- | --- |
| 已实现 | 代码、接口、成功/失败/边界测试、适用的重启/并发验证全部通过 |
| 部分实现 | 主路径可用，但仍有明确的缺口或缺少完整验证 |
| 待实现 | 需求和接口已定义，代码尚未形成可调用能力 |
| 阻塞 | 前置接口或外部环境未满足，且负责人已经提交阻塞说明 |

### 4.2 每个任务的完成定义

每个任务提交时必须包含以下内容：

1. 设计说明：模块边界、数据结构、接口、错误码和事务边界。
2. 代码：实现文件和必要的单元或集成测试。
3. 测试证据：命令、通过数量、失败路径和边界条件。
4. 兼容性：旧格式、旧 API 或旧计划的处理策略。
5. 文档：对应 progress 文档、实施清单、capabilities 和用户可见行为说明。
6. 回滚方案：迁移、持久化格式或并发协议失败时的恢复办法。
7. 评审记录：至少一名其他成员完成代码审查。

## 五、成员 A 详细任务

成员 A 负责编译前端和优化器。A 的任务必须先稳定内部 AST、Scope、Plan 和诊断契约，再允许 B/C 消费。

### A1 X12 Token 级错误恢复与单语句多诊断

**目标**

让一次编译请求在不执行任何 SQL 的前提下返回多条独立诊断，并保证错误恢复不会生成可执行的成功计划。

**当前问题**

当前错误定位主要停留在语句级或单个错误，缺少 Token 级同步点和多诊断列表；错误 AST 可能被后续阶段错误接受。

**设计范围**

1. 在 Lexer 中记录 Token 起止位置、原始文本、错误类别和恢复后的下一个稳定 Token。
2. 在 Parser 中定义同步集合，包括分号、右括号、语句关键字和查询块边界。
3. 对错误语句生成带 `invalid` 标记的 AST 节点，禁止 Planner 生成可执行计划。
4. 诊断对象统一包含 `code`、`message`、`line`、`column`、`endLine`、`endColumn`、`statementIndex`、`stage` 和 `source`。
5. 保留原始 SQL 片段，不把纠错后的 SQL 自动提交给执行器。
6. 对多条语句逐个恢复，已成功编译的语句也不能因为后续错误而偷偷执行。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/sql/lexer.hpp` | Token 位置和词法错误结构 |
| `outputs/minisql-backend/src/sql/lexer.cpp` | 非法字符、未闭合字符串、数字和注释恢复 |
| `outputs/minisql-backend/include/minisql/sql/parser.hpp` | 诊断列表和同步集合 |
| `outputs/minisql-backend/src/sql/parser.cpp` | 错误节点、恢复点和多语句解析 |
| `outputs/minisql-backend/src/sql/planner.cpp` | 拒绝含 invalid 节点的 AST |
| `outputs/minisql-backend/src/server/database_main.cpp` | diagnostics 返回多诊断 |
| `outputs/minisql-backend/tests/diagnostics-smoke.mjs` | 新增多诊断和恢复测试 |
| `outputs/minisql-workbench/src/diagnostic-location.ts` | 多诊断位置映射 |
| `outputs/minisql-workbench/src/App.tsx` | 诊断列表和错误定位 |

**接口契约**

`diagnostics` 响应示例：

```json
{
  "success": false,
  "count": 2,
  "diagnostics": [
    {
      "code": 2001,
      "stage": "parser",
      "message": "Expected expression",
      "line": 1,
      "column": 15,
      "endLine": 1,
      "endColumn": 15,
      "statementIndex": 0
    }
  ]
}
```

**验证**

1. 两条错误和一条合法 SQL 混合输入，返回两条独立诊断。
2. 字符串中包含分号时不能误切语句。
3. 错误 AST 不得进入 Planner。
4. 恢复后继续解析后续语句，不能影响前面已解析语句的边界。
5. 诊断位置与原始源码一致。

**完成判据**

X12 专项测试通过，错误 AST 执行拒绝测试通过，工作台能够显示多条诊断并定位到原始行。

### A2 X13 可扩展 AST 与 Schema 迁移契约

**目标**

固定 AST、逻辑计划和 Schema 的版本化规则，使新节点、新列类型和新迁移脚本能够被旧版本明确接受、拒绝或迁移。

**设计范围**

1. AST 和 Plan JSON 增加稳定的 `nodeVersion`、`planVersion`、`schemaVersion` 和 `producerVersion`。
2. 未知主版本必须拒绝；同一主版本内的小版本允许向后兼容读取。
3. 新 AST 节点交给旧 Planner 时返回明确错误，不生成空计划或默认节点。
4. 列、表、索引和约束使用稳定对象身份，避免仅依赖数组下标。
5. 定义 Schema 迁移脚本格式：源版本、目标版本、是否可逆、前置校验、执行动作和失败恢复点。
6. 迁移不得静默改变列类型、NULL 语义、约束或索引定义。
7. 增加迁移失败回滚点，失败后原数据库保持可打开状态。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/sql/serialization.hpp` | AST/Plan 版本和未知节点校验 |
| `outputs/minisql-backend/src/sql/planner.cpp` | 稳定对象身份和计划版本 |
| `outputs/minisql-backend/src/catalog/persistent_catalog.cpp` | Schema 版本和迁移入口 |
| `outputs/minisql-backend/src/execution/database.cpp` | 迁移执行、失败恢复和重启校验 |
| `outputs/minisql-backend/tests/planner-regression.mjs` | 新旧节点和版本拒绝测试 |
| `outputs/minisql-backend/tests/backup-smoke.mjs` | 迁移链与回滚测试 |

**验证**

1. AST/Plan 序列化往返保持一致。
2. 未知节点版本明确拒绝。
3. 旧 Planner 收到新节点不会返回成功空计划。
4. Schema 迁移成功后可重启读取。
5. 迁移中途失败后原数据库仍可打开。
6. 迁移不能静默丢失约束、索引或默认值。

**完成判据**

X13 版本拒绝、往返、迁移成功和迁移失败回滚测试全部通过。

### A3 X09 派生表、作用域和关联计划

**目标**

补齐派生表、未限定名相关作用域以及 Apply/SemiJoin 计划，保证复杂子查询在语义和结果上与等价查询一致。

**设计范围**

1. FROM 项支持派生表，并要求显式别名；重复输出列名报歧义。
2. 每个查询块维护独立 Scope，查找顺序为当前块列、当前块别名、外层相关 Scope。
3. 未限定名在多层作用域中按固定规则解析，禁止通过字符串替换实现相关引用。
4. 非相关子查询允许缓存；相关子查询按外层行绑定执行。
5. 先实现正确性优先的 Apply，再增加 SemiJoin/AntiJoin 和保守去相关规则。
6. 子查询含聚合、DISTINCT、LIMIT 或相关引用时，优化器默认不跨越边界改写。
7. UPDATE/DELETE 中的子查询必须使用与 SELECT 相同的 Scope 和 NULL 三值逻辑。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/sql/ast.hpp` | 派生表、Scope 和 Subquery 节点 |
| `outputs/minisql-backend/src/sql/parser.cpp` | 派生表语法和嵌套查询块 |
| `outputs/minisql-backend/src/sql/planner.cpp` | Scope 绑定、对象身份和 Apply/SemiJoin 计划 |
| `outputs/minisql-backend/src/optimizer/optimizer.cpp` | 关联改写和保守边界 |
| `outputs/minisql-backend/src/execution/database.cpp` | Apply/SemiJoin 执行 |
| `outputs/minisql-backend/tests/subquery-smoke.mjs` | 派生表和三层作用域测试 |

**验证**

1. 派生表别名、重复列和嵌套派生表。
2. 未限定名在外层和当前层同名时按规则解析。
3. IN/NOT IN、EXISTS、标量子查询的 NULL 和空集语义。
4. 标量子查询多行或多列明确报错。
5. 相关子查询在 UPDATE/DELETE 中结果正确。
6. Apply/SemiJoin 改写前后行数、重复行和 NULL 补值一致。

**完成判据**

X09 全量专项通过，并与 B 的 IndexScan、HashJoin 和事务测试完成组合回归。

### A4 X18 统计、成本模型和确定性计划选择

**目标**

建立可比较的成本模型，让相同输入和相同统计在重复编译时产生确定性计划，并能在 EXPLAIN 中区分估计来源和实际来源。

**设计范围**

1. 统计信息增加表行数、页数、列 distinct、NULL 比例、最小/最大值和可选直方图。
2. 统计刷新使用显式 `ANALYZE` 或受控后台任务，记录刷新时间和统计版本。
3. 成本模型至少覆盖 SeqScan、IndexScan、NestedLoopJoin、HashJoin 和 Sort。
4. 缺失统计使用有界默认值，并在计划中标记 `statsSource`。
5. 相同成本使用固定规则决胜，不能依赖容器迭代顺序或随机值。
6. EXPLAIN 返回估计行数、估计页数、估计成本、访问路径和过滤条件。
7. EXPLAIN ANALYZE 只读运行，返回实际行数、实际页读写和耗时。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/src/execution/database.cpp` | 统计刷新、成本计算和计划选择 |
| `outputs/minisql-backend/src/optimizer/optimizer.cpp` | 候选计划比较和确定性规则 |
| `outputs/minisql-backend/src/sql/planner.cpp` | IndexScan、Join 和 Sort 计划 |
| `outputs/minisql-backend/tests/statistics-smoke.mjs` | 统计字段和刷新测试 |
| `outputs/minisql-backend/tests/explain-smoke.mjs` | 估计/实际来源测试 |

**验证**

1. 高选择率和低选择率过滤。
2. 空表、缺失统计、统计过期和索引不存在。
3. 相同成本多次编译计划完全一致。
4. EXPLAIN 不执行目标写语句。
5. EXPLAIN ANALYZE 实际行数与执行结果一致。
6. 估计读页和实际读页差距有记录，不伪造固定加速倍数。

**完成判据**

X18 专项通过，EXPLAIN/EXPLAIN ANALYZE 字段进入 HTTP 契约并可由工作台展示。

### A5 编译与优化集成门禁

**目标**

保证 A 组改动不会破坏执行器和存储层。

**固定回归命令**

```powershell
cd outputs\minisql-backend
node tests\parser-regression.mjs
node tests\planner-regression.mjs
node tests\diagnostics-smoke.mjs
node tests\subquery-smoke.mjs
node tests\statistics-smoke.mjs
node tests\explain-smoke.mjs
.\bin\planner_contract.exe
.\bin\optimizer_contract.exe
```

**交付物**

1. X12、X13、X09、X18 的 progress 文档。
2. AST/Plan/Schema 版本迁移说明。
3. 编译器错误码表和多诊断协议。
4. 成本模型公式、默认值和确定性决胜规则。

## 六、成员 B 详细任务

成员 B 负责存储、索引、事务、恢复和外部执行资源。B 组修改会直接影响数据安全，所有持久化格式变更必须先写兼容和回滚方案。

### B1 X20 页级 B+ 树索引

**目标**

把当前“整棵树分页镜像”升级为节点级持久化，实现子页指针遍历、叶链范围扫描和删除后的借位/合并。

**设计范围**

1. 定义稳定索引页类型编号，例如 `INDEX_META`、`INDEX_INTERNAL`、`INDEX_LEAF`。
2. 索引页头记录页类型、树 id、节点层级、键数量、父页、左右兄弟和校验值。
3. 内部节点保存分隔键和子页指针，叶节点保存排序键、RowId 和相邻叶指针。
4. IndexScan 按根页到叶页逐页遍历，不再依赖整树内存镜像。
5. 插入分裂向父节点递归；删除低于阈值时先借位，再合并。
6. 索引页变更与堆页变更进入同一 WAL 批次，事务失败不能留下孤立索引项。
7. 增加索引结构校验命令，检测页类型、兄弟指针、键顺序和叶链完整性。
8. 索引损坏时明确拒绝或从堆表重建，不能静默返回错误结果。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/storage/bplus_tree.hpp` | 节点页结构和公开接口 |
| `outputs/minisql-backend/src/storage/bplus_tree.cpp` | 节点序列化、分裂、借位、合并和遍历 |
| `outputs/minisql-backend/src/storage/page_file.cpp` | 索引页类型和页校验 |
| `outputs/minisql-backend/src/execution/database.cpp` | 索引维护和 IndexScan |
| `outputs/minisql-backend/tests/index-smoke.mjs` | 页级分裂、合并、重启和范围测试 |
| `outputs/minisql-backend/tests/bplus_tree_contract.cpp` | 节点结构契约 |

**验证**

1. 根分裂和多级树。
2. 重复键、NULL、复合键和前缀范围。
3. 删除触发借位和合并。
4. UPDATE 键变化后索引与全扫一致。
5. 事务回滚后索引恢复。
6. 跨进程重启后叶链和根页可恢复。
7. 索引页类型、兄弟指针和键顺序损坏时明确拒绝。

**完成判据**

X20 全量专项通过，B+ 树契约和 `database-http.mjs` 回归通过。

### B2 X22 后台自动检查点与 WAL 截止位置

**目标**

把当前提交事件驱动的自动检查点扩展为后台可调度策略，并固定 WAL 截止位置语义。

**设计范围**

1. 保留写语句数、WAL 字节、脏页数量、脏页比例和时间窗口五类阈值。
2. 增加后台检查点调度器，但不能在活动事务提交点之前截断未提交日志。
3. WAL 记录增加提交序号、事务 id、起始 LSN、结束 LSN 和恢复起点。
4. 检查点记录恢复起点、脏页水位、目录版本和索引版本。
5. 日志回收只能删除已经落盘且不再被活动事务、备份或恢复所需的记录。
6. 支持 WAL 非零截止位置重做；恢复必须区分已提交、未提交和半条日志。
7. 统计接口暴露后台调度状态、上次检查点、当前 WAL 和脏页水位。
8. 故障注入覆盖日志落盘前、数据落盘前、提交记录后和检查点中途。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/storage/page_file.hpp` | WAL 截止位置和检查点元数据 |
| `outputs/minisql-backend/src/storage/page_file.cpp` | 后台检查点、日志保留和恢复 |
| `outputs/minisql-backend/src/execution/database.cpp` | 调度器、阈值和统计 |
| `outputs/minisql-backend/tests/auto-checkpoint-smoke.mjs` | 阈值和后台调度测试 |
| `outputs/minisql-backend/tests/x22-fault-injection.mjs` | 跨进程故障注入 |
| `outputs/minisql-backend/tests/journal-process.mjs` | WAL 重做和截断测试 |

**验证**

1. 五类阈值分别触发且触发原因准确。
2. 活动事务期间不得清理仍需要的日志。
3. 检查点中途退出后可重复恢复。
4. 提交记录已落盘但数据页未落盘时，恢复后已提交行存在。
5. 日志损坏、半条日志和错误身份明确拒绝。
6. 非零 WAL 截止位置重做后行、目录和索引一致。

**完成判据**

X22 全量专项、跨进程重启和备份恢复回归通过。

### B3 X25 执行器级流式结果与资源预算

**目标**

把当前“结果先物化、bridge 再分帧”的传输层流式，升级为执行器级迭代和可取消的资源预算。

**设计范围**

1. 定义执行器 `RowStream` 接口，至少支持 `next`、`cancel`、`close` 和 `resourceUsage`。
2. SeqScan、Filter、Project、Sort、Aggregate、Join 逐步改成流式输入或显式物化节点。
3. Sort/Aggregate 使用有界内存 run，输入从扫描算子直接写入 run，不再先构造完整输入数组。
4. 结果行数、临时文件字节、排序 run 数和聚合状态数分别设置预算。
5. 达到预算时立即停止扫描并释放 pin、临时文件和取消句柄。
6. HTTP bridge 使用逐行协议和客户端读取确认，避免服务端无限超前生产。
7. 客户端断开、取消、超时和写入失败时返回明确状态；写事务取消必须标记提交状态未知或已回滚。
8. 工作台提供流式结果状态、取消按钮、预算错误和临时资源清理提示。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/include/minisql/execution/executor.hpp` | RowStream 和资源预算接口 |
| `outputs/minisql-backend/src/execution/database.cpp` | 扫描、排序、聚合和连接流式执行 |
| `outputs/minisql-backend/src/execution/external_sort.hpp` | 有界 run 和流式输入 |
| `outputs/minisql-backend/src/server/database_main.cpp` | 逐行 session 协议 |
| `outputs/minisql-backend/scripts/database-bridge.mjs` | NDJSON 背压和取消 |
| `outputs/minisql-workbench/src/client.ts` | 流式消费和错误恢复 |
| `outputs/minisql-backend/tests/x25-stream-http.mjs` | 流式分帧、断开和资源回收 |

**验证**

1. 大结果集分块返回，行顺序和值与内存方案一致。
2. 排序和聚合输入超过预算时进入外部 run。
3. 中途取消后临时文件和 pin 全部释放。
4. 客户端慢读时服务端等待背压，不无限增长内存。
5. 行数、临时文件、单分组和资源预算超限返回明确错误。
6. 写事务取消不自动重试，提交状态明确。

**完成判据**

X25 全量压力测试、跨进程取消恢复和前端流式状态测试通过。

### B4 X26 在线备份与迁移回滚

**目标**

支持备份期间在线读写一致性快照、非零 WAL 截止位置恢复和迁移失败回滚目录。

**设计范围**

1. 备份开始记录页文件水位、目录版本、索引版本和 WAL LSN。
2. 使用写时复制、日志截取或一致性快照保证备份期间读写不破坏一致性。
3. manifest 记录快照 LSN、WAL 截止位置、页校验、目录版本和迁移版本。
4. 恢复先校验整条备份链，再写临时目录，最后原子替换数据库。
5. 非零 WAL 截止位置恢复必须从对应 LSN 重做，并支持重复恢复。
6. 迁移脚本失败时保留原库和回滚目录，不允许半迁移数据库对用户开放。
7. 备份列表暴露快照时间、WAL 位置、链深度和校验状态。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/scripts/database-bridge.mjs` | 在线备份、manifest 和恢复入口 |
| `outputs/minisql-backend/src/storage/page_file.cpp` | 快照水位和 WAL LSN |
| `outputs/minisql-backend/src/catalog/persistent_catalog.cpp` | 目录版本和迁移状态 |
| `outputs/minisql-backend/src/execution/database.cpp` | 一致性快照和恢复校验 |
| `outputs/minisql-backend/tests/backup-smoke.mjs` | 在线备份、链恢复和失败回滚 |

**验证**

1. 备份期间并发读写后恢复行、约束和索引一致。
2. 截断 WAL、损坏页、错误版本和缺文件明确拒绝。
3. 重复恢复幂等。
4. 非零 WAL 截止位置恢复后数据一致。
5. 迁移失败后原数据库仍可打开，回滚目录可检查。

**完成判据**

X26 全量专项和跨进程重启回归通过。

### B5 存储与恢复集成门禁

**固定回归命令**

```powershell
cd outputs\minisql-backend
node tests\index-smoke.mjs
node tests\auto-checkpoint-smoke.mjs
node tests\x22-fault-injection.mjs
node tests\journal-process.mjs
node tests\x25-stream-http.mjs
node tests\backup-smoke.mjs
node tests\database-http.mjs
.\bin\bplus_tree_contract.exe
```

**交付物**

1. 索引页格式规范。
2. WAL 记录格式、LSN 和检查点恢复规范。
3. 执行器 RowStream 与资源预算协议。
4. 在线备份和迁移回滚设计。
5. 持久化格式兼容表。

## 七、成员 C 详细任务

成员 C 负责权限、审计、工作台、Fuzz 和最终验收。C 组不能绕过后端接口直接修改 A/B 的存储结构；发现接口缺口时提交契约变更请求。

### C1 X24 权限、角色与审计闭环

**目标**

让 CLI 和 HTTP 使用同一套权限检查，权限数据进入页式 Catalog，并提供原子 GRANT/REVOKE 和工作台管理界面。

**设计范围**

1. 把用户、角色、角色继承、对象授权和权限版本迁移到页式 Catalog 或受控系统表。
2. 密码继续使用随机盐和不可逆摘要，任何接口不返回密码散列。
3. CLI 和 HTTP 共用 `Authorizer` 接口，禁止绕过 bridge 直接调用执行器取得权限。
4. 权限检查发生在解析绑定后、计划生成前，错误信息不泄露无权对象存在。
5. 提供 `GRANT`、`REVOKE`、`CREATE USER`、`CREATE ROLE` 等原子资源接口。
6. 权限变更增加版本号，旧计划执行前重新校验权限版本。
7. 审计记录用户、会话、对象、操作、结果、错误码、影响行数和耗时。
8. 工作台增加用户、角色、对象授权、审计过滤和撤权即时生效提示。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/scripts/access-catalog.mjs` | 页式 Catalog 适配和原子授权接口 |
| `outputs/minisql-backend/scripts/database-bridge.mjs` | CLI/API 共用鉴权、GRANT/REVOKE 和审计 |
| `outputs/minisql-backend/src/catalog/persistent_catalog.cpp` | 权限对象持久化 |
| `outputs/minisql-backend/src/sql/planner.cpp` | 绑定后权限检查 |
| `outputs/minisql-workbench/src/App.tsx` | 权限管理入口 |
| `outputs/minisql-workbench/src/types.ts` | 权限和审计响应类型 |
| `outputs/minisql-backend/tests/access-control-http.mjs` | 权限闭环测试 |

**验证**

1. 角色继承和对象级授权。
2. 撤权后旧计划下一次执行立即失败。
3. 未授权对象不出现在 Catalog、statistics 或诊断中。
4. 跨会话身份复用被拒绝。
5. 密码错误、未知用户和无 CONNECT 权限统一拒绝。
6. 审计过滤和敏感值脱敏。

**完成判据**

X24 全量专项、CLI 权限测试和工作台权限流程测试通过。

### C2 工作台最终收口

**目标**

把后端新增的权限、并发、流式、备份、迁移和资源预算字段完整展示，不把缺失字段伪装成零值。

**设计范围**

1. 多会话状态面板：会话 id、用户、事务状态、锁等待、活动请求和最后活动时间。
2. 事务与取消：锁等待提示、取消按钮、提交状态未知和回滚提示。
3. 编译诊断：多条诊断列表、错误范围、智能纠错建议不自动执行。
4. Plan/EXPLAIN：访问路径、成本来源、实际行数、实际读页和估计差异。
5. 权限与审计：用户、角色、对象授权、撤权即时生效和审计过滤。
6. 备份与迁移：备份链、快照时间、WAL 位置、恢复结果和失败回滚。
7. 资源预算：结果行数、临时文件、取消状态和预算错误。
8. 所有新字段先进入 `types.ts`，再进入结果、Plan、Diagnostics 和设置面板。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-workbench/src/types.ts` | 后端响应类型 |
| `outputs/minisql-workbench/src/App.tsx` | 会话、权限、审计和备份视图 |
| `outputs/minisql-workbench/src/CompilerViews.tsx` | Plan 和诊断展示 |
| `outputs/minisql-workbench/src/Results.tsx` | 流式结果和资源状态 |
| `outputs/minisql-workbench/src/StorageStatistics.tsx` | 索引、WAL、检查和备份统计 |
| `outputs/minisql-workbench/tests/*.cjs` | 真实浏览器回归 |

**验证**

1. 桌面和移动视口无文字溢出或遮挡。
2. 多会话、锁等待和取消状态可复现。
3. 权限错误不泄露目标对象名称。
4. 备份、恢复和迁移失败状态清晰。
5. 缺失字段显示“未提供”，不显示伪零值。

**完成判据**

工作台 UI01-UI10 验收通过，浏览器回归覆盖成功、失败、取消、超时和恢复场景。

### C3 X27 DDL/DML Fuzz 与长期回归

**目标**

把当前以 SELECT 为主的差分测试扩展为覆盖 DDL、DML、事务、索引和恢复的状态机测试。

**设计范围**

1. 固定种子、Schema、数据集、配置和资源上限。
2. 生成合法 CREATE、DROP、ALTER、INSERT、UPDATE、DELETE、JOIN、NULL 和事务序列。
3. 非法变异保留原始样本和分类，不把所有变异都判为非法。
4. 统计 Crash、Wrong Accept、Wrong Reject、Error Location、Timeout 和 Resource Limit。
5. 故障样本最小化后重新确认，保存可复现回归用例。
6. 差分测试只比较双方共同支持且语义一致的 SQL 子集。
7. 建立 CI 或可重复的夜间回归脚本，输出版本、种子、语料 SHA-256 和失败样本。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/minisql-backend/tests/fuzz-model.mjs` | DDL/DML 状态机 |
| `outputs/minisql-backend/tests/fuzz-differential.mjs` | 状态机差分和故障分类 |
| `outputs/minisql-backend/tests/fuzz-process.mjs` | 跨进程隔离和超时 |
| `outputs/minisql-backend/tests/fuzz/*` | 固定种子语料和回归样本 |
| `.github/workflows/ci.yml` | CI 构建和回归 |

**验证**

1. 同种子重放语料一致。
2. 合法变异不误报。
3. UPDATE/DELETE/多行 INSERT/JOIN/NULL/事务/索引覆盖。
4. 失败样本最小化后仍能复现。
5. 超时和资源限制单独计数。

**完成判据**

X27 固定种子长跑、CI 回归和失败样本重放通过。

### C4 X01-X27 全量组合验收

**目标**

建立需求、实现、测试、接口和证据的一一对应关系，输出最终验收报告。

**设计范围**

1. 建立 feature matrix，列出 X01-X27 的需求、负责人、代码路径、测试命令、证据文件和状态。
2. 组合验收覆盖 JOIN、WHERE、GROUP BY、HAVING、DISTINCT、ORDER BY、LIMIT、NULL、UPDATE、索引、WAL、事务和权限。
3. 每条优化规则至少覆盖适用和不适用案例。
4. HTTP、前端、CLI、跨进程重启和并发场景必须有证据。
5. 未实现项明确写“未启用”或“部分实现”，不能用空实现或固定样例输出替代。
6. 最终报告包含构建版本、提交哈希、运行环境、测试数量和已知风险。

**主要文件**

| 文件 | 修改内容 |
| --- | --- |
| `outputs/MiniSQL扩展功能实施清单_V1.0.md` | 状态和证据更新 |
| `outputs/V3当前实现状态.md` | 当前能力收口 |
| `outputs/V3最终验收报告.md` | 新增最终报告 |
| `outputs/minisql-backend/docs/*-progress.md` | 各模块证据 |
| `.github/workflows/ci.yml` | 自动执行核心回归 |

**完成判据**

X01-X27 全部有可复现证据，组合验收通过，工作区干净，最终报告可由他人按命令复跑。

## 八、跨组接口与协作规则

### 8.1 接口冻结顺序

| 接口 | 提供方 | 消费方 | 冻结时间 | 变更规则 |
| --- | --- | --- | --- | --- |
| AST/Plan JSON 版本 | 成员 A | 成员 B、成员 C | 第一阶段结束 | 主版本变更必须提供拒绝或迁移测试 |
| 诊断多结果协议 | 成员 A | 成员 C | 第一阶段结束 | 字段新增向后兼容，删除必须升级版本 |
| Schema 迁移契约 | 成员 A | 成员 B、成员 C | 第二阶段开始 | 必须包含回滚点和兼容测试 |
| IndexScan 计划字段 | 成员 A、成员 B | 成员 C | 第二阶段开始 | EXPLAIN 与执行字段必须一致 |
| RowStream 协议 | 成员 B | 成员 C | 第二阶段开始 | 取消和资源状态不可省略 |
| WAL/LSN 统计字段 | 成员 B | 成员 C | 第二阶段结束 | 字段含义变化必须更新统计文档 |
| 权限和审计响应 | 成员 C | 成员 A、成员 B | 第一阶段结束 | 对象身份变化必须同步权限版本 |
| capabilities 字段 | 所有成员 | 工作台和验收 | 每次合入 | 新能力必须同时更新前端类型和文档 |

### 8.2 Git 与评审规则

1. `main` 分支只接受通过 CI 或本地完整回归的合并。
2. 分支命名使用 `feature/x12-parser-recovery`、`fix/x20-leaf-merge`、`docs/x24-permission-contract` 等格式。
3. 每个任务一个提交序列，提交信息包含任务编号和范围。
4. 每个任务至少由另外一名成员审查，跨组接口变更必须由两个消费方确认。
5. 禁止直接提交构建产物、数据库文件、取消文件、WAL、备份文件、测试 artifacts 或密码。
6. 任务完成后必须同步 progress 文档、实施清单和 capabilities。
7. 出现阻塞时提交阻塞说明，包括已尝试方案、最小复现、影响范围和需要的接口决定。

### 8.3 每周检查点

| 时间 | 检查内容 | 负责人 |
| --- | --- | --- |
| 周一 | 冻结本周接口和验收命令 | 三人共同 |
| 周三 | 中期集成，检查跨组契约和回归失败 | 轮值负责人 |
| 周五 | 提交任务进度、测试证据和下周风险 | 三人共同 |
| 每个阶段末 | 代码审查、全量回归、文档和 tag | 成员 C 统筹 |

## 九、建议排期

以下排期按四个阶段安排，具体日期按课程节点调整。

### 第一阶段：编译契约与安全基线

目标：完成 X12、X13、X24 的接口和核心实现，冻结诊断、版本、权限和审计协议。

| 任务 | 负责人 | 交付物 | 退出条件 |
| --- | --- | --- | --- |
| X12 多诊断和恢复 | 成员 A | Token 位置、同步集合、诊断列表 | 多诊断测试通过 |
| X13 AST/Plan/Schema 版本 | 成员 A | 版本字段、拒绝/迁移契约 | 未知版本拒绝和往返通过 |
| X24 权限目录和原子授权 | 成员 C | CLI/API 共用鉴权、GRANT/REVOKE | 权限专项通过 |
| X20 页格式设计 | 成员 B | 索引页头和节点格式 | 设计评审通过 |
| X22 WAL LSN 设计 | 成员 B | WAL 记录和检查点恢复规范 | 设计评审通过 |

### 第二阶段：核心实现

目标：完成 X09、X18、X20、X22、X25 和 X26 的主路径。

| 任务 | 负责人 | 交付物 | 退出条件 |
| --- | --- | --- | --- |
| 派生表和 Apply/SemiJoin | 成员 A | Scope、计划和执行 | X09 专项通过 |
| 统计和成本模型 | 成员 A | 统计刷新、成本公式、确定性计划 | X18 专项通过 |
| 页级 B+ 树 | 成员 B | 节点遍历、分裂、借位、合并 | X20 专项通过 |
| 后台检查和 WAL LSN | 成员 B | 调度器、日志保留、非零 LSN | X22 专项通过 |
| RowStream 和资源预算 | 成员 B | 执行器迭代、背压、取消 | X25 主路径通过 |
| 在线备份和迁移回滚 | 成员 B | 一致性快照、回滚目录 | X26 主路径通过 |
| 权限 UI 和审计查询 | 成员 C | 用户、角色、对象授权、审计过滤 | 浏览器回归通过 |

### 第三阶段：工作台和 Fuzz 收口

目标：把后端能力完整暴露到工作台，并建立 DDL/DML 状态机测试。

| 任务 | 负责人 | 交付物 | 退出条件 |
| --- | --- | --- | --- |
| 多会话和锁等待 UI | 成员 C | 会话面板、取消和状态提示 | UI01-UI06 通过 |
| Plan/EXPLAIN 字段展示 | 成员 C、成员 A | 访问路径、成本来源、实际统计 | UI07-UI08 通过 |
| 备份迁移和资源预算 UI | 成员 C、成员 B | 备份链、恢复结果、预算错误 | UI09-UI10 通过 |
| DDL/DML Fuzz | 成员 C | 状态机、差分、最小化和 CI | X27 长跑通过 |

### 第四阶段：全量验收

目标：完成 X01-X27 组合验收和最终报告。

| 任务 | 负责人 | 交付物 | 退出条件 |
| --- | --- | --- | --- |
| 模块回归 | 三人共同 | 各专项测试证据 | 所有已启用模块通过 |
| 跨进程和并发回归 | 成员 B、成员 C | 重启、故障、锁等待和取消证据 | 跨进程矩阵通过 |
| HTTP 和前端回归 | 成员 C | 真实浏览器和 API 证据 | 工作台验收通过 |
| feature matrix | 成员 C | X01-X27 证据表 | 每项有代码、命令和结果 |
| 最终报告和 tag | 三人共同 | 最终验收报告、提交哈希、tag | 可复现 |

## 十、风险与应对

| 风险 | 影响 | 触发信号 | 应对 |
| --- | --- | --- | --- |
| WAL 语义改动导致恢复错误 | 数据丢失或重复恢复 | 非零 LSN 恢复失败、重复恢复不一致 | 先冻结格式和恢复点，再做故障注入 |
| 索引页格式与旧数据不兼容 | 数据库无法打开 | 旧索引页读取失败 | 提供版本迁移、重建和回滚路径 |
| 流式执行导致结果顺序变化 | 用户结果不一致 | 流式和内存结果差异 | 固定比较器和帧顺序测试 |
| 权限目录迁移泄露数据 | 越权访问 | Catalog 或诊断出现不可见对象 | 先做权限版本和拒绝测试，再迁移 |
| 前端与后端字段漂移 | 工作台显示伪数据 | TypeScript 类型与 HTTP 响应不一致 | 先冻结 `types.ts`，缺失字段显示未提供 |
| Fuzz 把合法变异判为错误 | 误报和返工 | Wrong Reject 增长 | 保留样本、人工复核和参考引擎限定 |
| 三人并行修改同一文件 | 合并冲突和行为回归 | 同一函数频繁冲突 | 按接口冻结、分支隔离和每周集成 |
| 验收只在单机通过 | 环境差异导致失败 | 换机器无法复现 | CI 固定依赖、种子、版本和命令 |

## 十一、交付物清单

1. X12 多诊断和错误恢复设计及测试。
2. X13 AST/Plan/Schema 版本与迁移契约。
3. X09 派生表、作用域和 Apply/SemiJoin 实现。
4. X18 统计、成本模型和确定性计划。
5. X20 页级 B+ 树和索引页格式。
6. X22 后台检查和 WAL 截止位置。
7. X24 页式权限目录、CLI/API 共用鉴权和权限 UI。
8. X25 RowStream、资源预算、背压和提前停止。
9. X26 在线备份、非零 WAL 恢复和迁移回滚。
10. X27 DDL/DML Fuzz、CI 和长期回归。
11. 工作台 UI01-UI10 验收证据。
12. X01-X27 feature matrix。
13. V3 最终验收报告。
14. 代码提交、版本 tag、构建命令和复现实验记录。

## 十二、任务开始前的填写项

在正式执行前，三名成员需要确认以下信息并写回本任务书：

| 项目 | 填写内容 |
| --- | --- |
| 成员 A 姓名 |  |
| 成员 B 姓名 |  |
| 成员 C 姓名 |  |
| 课程提交日期 |  |
| 可用的持续集成平台 |  |
| 允许使用的第三方库 |  |
| 数据库文件格式兼容要求 |  |
| 演示环境操作系统 |  |
| 最终答辩日期 |  |

未填写以上信息不影响任务按模块推进，但会影响排期、CI 选型和最终验收时间。
