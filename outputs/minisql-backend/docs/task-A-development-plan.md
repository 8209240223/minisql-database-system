# 成员 A 开发计划（函数级）：编译前端与优化器

对应：《MiniSQL_V3_未完成功能三人分工详细设计任务书.md》第五/九/十节
主责：**X12、X13、X09 剩余、X18 剩余** + **A5 编译器/优化集成门禁**
作者身份：anyu999（本地提交，推送由用户另行操作）
代码根：`outputs/minisql-backend`

---

## 0. 基线锁定（第一个动作）

跑 A5 固定回归，记录每项当前通过数/失败点作基线：

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

同时把 `diagnostics()` 当前 JSON（见 `execution/database.cpp:1332-1375`）作契约基线快照。

---

## 1. 现状基线（函数级，已核实）

### X12 编译器错误恢复 —— 差距
| 位置 | 现状 | 缺口 |
| --- | --- | --- |
| `sql/lexer.cpp` `fail()` L18-20、L23-27、L35-67 | 遇词法错误**立即 throw Lexical**，链断 | 无「恢复跳过到稳定 Token」；无词法错误类别字段 |
| `sql/lexer.hpp` `Token` L6 | `type/lexeme/location` | 无结束位置、无原始文本/错误类别 |
| `sql/parser.cpp` `take()/expect()/identifier()/literal()` L10-18 | 每题**抛即止** | 无多诊断收集；无同步集合（sync set） |
| `sql/parser.hpp` `Statement` | 语句级 `location` | 无 `invalid` 标记 → 错误 AST 无拒收标识 |
| `execution/database.cpp` `diagnostics()` L1332-1375 | 语句级多诊断，字段 `success/stage/code/message/line/column/recoverable` | 无 `endLine/endColumn/statementIndex` |
| `sql/planner.cpp` `build()` L216-227 | `invalid()` 抛 Internal | 无「AST 含 invalid 即拒、不产可执行计划」的显式闸门 |

### X13 AST/Plan/Schema 版本化 —— 差距
| 位置 | 现状 | 缺口 |
| --- | --- | --- |
| `include/minisql/sql/serialization.hpp` | 有 version 包装 + 往返 | 无 `nodeVersion/planVersion/schemaVersion/producerVersion`；未知主版本不拒绝 |
| `sql/planner.cpp` L450-480（`compilePlans`） | 顺序编译 | 输出计划无稳定版本字段 |
| `catalog/persistent_catalog.cpp` | 目录持久化 | 无 `schemaVersion` 迁移入口 |
| `execution/database.cpp` | 打开库 | 无迁移执行/失败回滚/重启校验 |

### X09 子查询/作用域 —— 差距
| 位置 | 现状 | 缺口 |
| --- | --- | --- |
| `sql/parser.hpp` `Expr` L9 → `subquerySql` | **子查询以字符串存**（违背任务书「禁字符串替换」） | 需改为结构化对象身份（DerivedTable/Subquery 节点） |
| `sql/parser.cpp` `select(false)` L235 | IN (SELECT…) 解析后存 `subquerySql` | 无 `Scope`；FROM 无派生表 |
| `sql/planner.cpp` `bindExpression()` L58-67 | 直接把 Identifier 绑到 catalog 列 | 无当前块/外层作用域链；未限定名无固定解析规则 |
| `sql/planner.hpp` `Join` L14 | `table/alias/on/{left,right}` | 无 Apply/SemiJoin 计划 |
| `optimizer/optimizer.cpp` L22-35 | 支持 Exists/Scalar/InSubquery 重写 | 无去相关/Apply 改写；无跨边界保守规则 |

### X18 统计/成本 —— 差距
| 位置 | 现状 | 缺口 |
| --- | --- | --- |
| `execution/database.cpp` `statistics()` | 已有表/列行数/页数/distinct/NULL | 无直方图、无 `ANALYZE` 刷新、无统计版本/刷新时间 |
| `optimizer/optimizer.cpp` 主循环 L368-404 | 单一 rewrite 固定点 | 无候选计划比较、无成本公式、无确定性决胜 |
| `sql/planner.cpp` | 出 IndexScan/Join/Sort 计划 | 计划缺 `statsSource`/估计行数/页数/成本字段 |
| `explain` | 已有 stats-v1 估计 + EXPLAIN ANALYZE 实际 | 估计来源/实际来源未区分，无完整成本模型 |

---

## 2. 分阶段规划（函数级）

> 顺序遵循接口冻结规则（任务书 8.1）：Phase0/1 先冻结 A 对外契约，再进实现；IndexScan 计划字段与 B 协商后再做。

### Phase 0 —— 基线 + 契约冻结
- [ ] 跑 §0 基线回归，记录通过数。
- [ ] R1 扩展 `SourceLocation` → 增 `endLine/endColumn`（默认回退起止）。影响文件：`common/error.hpp`；同步 `ast`/`Expr` 序列化字段读取。
- [ ] R2 定义统一 `Diagnostic` 结构（code/message/line/column/endLine/endColumn/statementIndex/stage/source）与 JSON 契约，写 `docs/diagnostics-progress.md`。
- [ ] R3 `serialization.hpp` 定义版本常量：`AST_NODE_VERSION/PLAN_VERSION/PRODUCER_VERSION/CATALOG_SCHEMA_VERSION`，主版本未知→拒绝，小版本→兼容读。
- **跨组交付**：诊断 JSON 快照、AST/Plan 版本字段 → 告知 B/C（types.ts 同步）。

### Phase 1 —— X12 Token 级错误恢复 + 单语句多诊断
- **1.1 lexer 恢复**（`lexer.hpp/.cpp`）
  - `Token` 增 `endLocation`；扫描器取消首个错误的 `fail()` throw，改为**记录诊断并跳到稳定的下一个 Token** 继续（`tokenize`/`scanTokens` 增诊断输出参数）。
  - 非法字符、未闭合字符串、非法数字、未闭合注释 → 各输出一条 `code=2001,stage=lexer` 诊断 + 恢复点；字符串内 `;` 不切语句。
- **1.2 parser 多诊断 + 同步**（`parser.hpp/.cpp`）
  - 定义 sync set：`;`、`)`、语句关键字（SELECT/INSERT/UPDATE/DELETE/CREATE/DROP/…）、查询块边界。
  - `take/expect/identifier/literal` 不再抛即止，改为收集诊断 + 按 sync set 跳到同步点继续本语句内解析，从而**单语句多诊断**。
  - 错误处生成带 `invalid=true` 的节点/Statement；多语句循环边界不被打乱。
- **1.3 planner 拒收**（`planner.cpp` `build()/compilePlans()`）：AST 含任何 `invalid` → 直接返回错误，**不产可执行计划、不产空计划/默认节点**。
- **1.4 服务端+工作台**：`database.cpp diagnostics()` 输出全字段（含 `statementIndex/endLine/endColumn`）；`database_main.cpp` 透传；workbench `diagnostic-location.ts` 多诊断映射 + `App.tsx` 列表定位。
- **1.5 测试**（`tests/diagnostics-smoke.mjs` 扩展）：
  - ① 2 错 + 1 对混合 → `count=2`、两条独立诊断、合法语句仍 `success:true`；
  - ② `'a;b'` 字符串含分号不误切；③ invalid AST 不进 Planner（执行拒绝测试）；
  - ④ 恢复后后续语句仍正确解析；⑤ 位置与源码逐字符一致（含 endLine/endColumn、statementIndex）。
- **退出**：X12 专项通过 + 错误 AST 执行拒绝测试通过 + 工作台多诊断定位可用。

### Phase 2 —— X13 Schema 迁移契约
- 2.1 `serialization.hpp`：未知主版本拒绝；同主版小版本兼容读（新旧节点往返测试）。
- 2.1 `serialization.hpp`：未知主版本拒绝；同主版小版本兼容读（新旧节点往返测试）。
- 2.2 `catalog/persistent_catalog.cpp`：`schemaVersion` 落盘 + 迁移入口（源/目标版本、可逆、前置校验、动作、失败恢复点）。
- 2.3 `execution/database.cpp`：迁移执行、失败回滚点、重启校验；迁移不得静默改列类型/NULL/约束/索引。
- 2.4 测试 `tests/planner-regression.mjs`（新旧节点、未知版本拒绝、往返）+ `tests/backup-smoke.mjs`（迁移链、失败回滚后库可打开）。
- **退出**：X13 版本拒绝 / 往返 / 迁移成功 / 迁移失败回滚全部通过。

### Phase 3 —— X09 派生表、作用域与 Apply/SemiJoin
- 3.1 `sql/ast.hpp`：`DerivedTable`/`Scope`/`Subquery` 结构化节点；`Expr` 移除 `subquerySql`，改为子查询对象身份；列/表/索引/约束用稳定对象身份（非数组下标）。
- 3.2 `sql/parser.cpp`：FROM 支持派生表（显式别名，重复输出列名报歧义）、嵌套查询块；`select()` 返回结构化子查询节点。
- 3.3 `sql/planner.cpp` `bindExpression()`/`build()`：建立逐查询块 `Scope` 链（当前列→当前别名→外层相关），未限定名按固定顺序解析（禁字符串替换）；生成 Apply/SemiJoin 计划。
- 3.4 `optimizer/optimizer.cpp`：先正确性优先 Apply，后 SemiJoin/AntiJoin + 保守去相关；子查询含聚合/DISTINCT/LIMIT/相关引用默认不跨边界改写；UPDATE/DELETE 用同一 Scope 与 NULL 三值逻辑。
- 3.5 `execution/database.cpp`：Apply/SemiJoin 执行（非相关可缓存，相关按外层行绑定）。
- 3.6 测试 `tests/subquery-smoke.mjs` 扩展：派生表别名/重复列/嵌套；三层作用域未限定名；IN/NOT IN/EXISTS/标量 NULL 与空集；标量多行/多列报错；UPDATE/DELETE 相关子查询；Apply 改写前后行数/重复/NULL 一致。
- **退出**：X09 专项通过与 B 的 IndexScan、HashJoin、事务测试组合回归（与 B 一起联调）。

### Phase 4 —— X18 统计、成本模型、确定性计划
- 4.1 `execution/database.cpp` `statistics()`：增列 distinct/NULL 比例/min/max/可选直方图；显式 `ANALYZE` 或受控后台刷新，记录刷新时间与统计版本；缺失统计用有界默认值并标 `statsSource`。
- 4.2 `optimizer/optimizer.cpp`：成本公式覆盖 SeqScan/IndexScan/NestedLoopJoin/HashJoin/Sort；候选计划比较 + 固定决胜规则（不依赖容器迭代顺序/随机值）；重复编译同统计→同计划。
- 4.3 `sql/planner.cpp`：IndexScan/Join/Sort 计划输出 `statsSource`、估计行数/页数/成本、访问路径、过滤条件。
- 4.4 explain：EXPLAIN 返回估计；EXPLAIN ANALYZE 只读返回实际行数/实际页读写/耗时；估计来源/实际来源区分。
- 4.5 测试 `tests/statistics-smoke.mjs`、`tests/explain-smoke.mjs`：高低选择率、空表/缺失统计/过期/索引不存在、同成本多次编译一致、EXPLAIN 不执行写语句、实际行数与执行一致、读页差有记录（不伪造加速倍数）。
- **依赖**：IndexScan 计划字段与 B(X20) 协商冻结（第二阶段开始），先接口后实现、复用现有点查。
- **退出**：X18 专项通过 + EXPLAIN 字段进入 HTTP 契约并可由工作台展示。

### Phase 5 —— A5 编译与优化集成门禁
- 全量跑 §0 固定回归全部通过；`planner_contract/optimizer_contract.exe` 无回归。
- 交付物：X12/X13/X09/X18 的 progress 文档；AST/Plan/Schema 版本迁移说明；编译错误码表 + 多诊断协议；成本模型公式/默认值/决胜规则。
- 分支合入前：每个任务一次提交序列（author=anyu999）+ 至少一名成员评审 + 全量回归。

---

## 3. 回归命令（每 Phase 结束必跑）

```powershell
cd outputs\minisql-backend
node tests\parser-regression.mjs tests\planner-regression.mjs tests\diagnostics-smoke.mjs
node tests\subquery-smoke.mjs tests\statistics-smoke.mjs tests\explain-smoke.mjs
.\bin\planner_contract.exe .\bin\optimizer_contract.exe
# 联调：node tests\database-http.mjs
```

## 4. 跨组接口与风险

| 依赖/风险 | 类型 | 对策 |
| --- | --- | --- |
| AST/Plan 结构改动影响 B/C | 雪崩 | Phase0 冻结版本契约 + 评审；下沉 B/C 前先同步 types.ts |
| IndexScan 计划字段（X18） | 依赖 B(X20) | 第二阶段冻结契约，先接口后实现，复用现有点查 |
| X09 去相关改写正确性 | 高 | 先正确性优先 Apply，再 SemiJoin/AntiJoin；严格 NULL/空集/标量报错测试 |
| Schema 迁移破坏可打开性 | 高 | 强制失败回滚点 + 失败后库可打开测试 |
| 统计/成本确定性 | 需求 | 固定决胜规则 + 同输入多次编译一致测试 |
| 单语句多诊断跨阶段消化 | 高 | synset 与 invalid 标记先行，Planner 拒收闸门配套 |

## 5. 提交与评审

- 分支：`feature/x12-parser-recovery` / `feature/x13-schema-migration` / `feature/x09-subquery-scope` / `feature/x18-cost-model`。
- 每个任务一个提交序列，author=anyu999；每任务至少一名成员评审；合入 `main` 前跑 §3 回归。

---

## 6. 本次提交（builder-A）执行记录

> 基于最新 main `97f50bd`，分支 `builder-A`（fast-forward 推送）。范围：**Phase 0 + Phase 1（X12）**。

### 6.0 基线核实（逐行确认，非推断）
- [x] `common/error.hpp` `SourceLocation` 仅 `line/column`，无 `endLine/endColumn`。
- [x] `sql/lexer.hpp` `Token` 仅 `type/lexeme/location`，无 `endLocation`；`tokenize` 纯 value 返回、无诊断输出参数。
- [x] `sql/lexer.cpp` 共 9 处 `fail()`（L26/40/47/51/55/59/61/64/66），全部立即 throw Lexical。
- [x] `sql/parser.cpp` `take/expect/identifier/literal` 抛 Syntax 即止。
- [x] `sql/parser.hpp` `Statement` 无 `invalid` 标记；`Expr` 子查询仍以 `subquerySql` 字符串存储。
- [x] `sql/planner.cpp` `build()` 无 invalid 拒收闸门；`bindExpression()` 无作用域链。
- [x] `include/minisql/sql/serialization.hpp` 无细分版本常量、无未知主版本拒绝。
- [x] `execution/database.cpp` `diagnostics()`（L1332-1376）语句级多诊断，缺 `endLine/endColumn/statementIndex`。

### 6.1 构建环境修复
- 根因：VS BuildTools 自带 vcpkg 在 Windows 上 install 引导阶段需要 `powershell-core`(pwsh)，但 `environment.ps1` 的 `VCPKG_FORCE_SYSTEM_BINARIES=1` 禁止 vcpkg 下载工具，而本机无系统 pwsh → `Could not fetch powershell-core`。
- 修复：[scripts/environment.ps1](../scripts/environment.ps1) 移除 FORCE 标志（仅移除强制，CMake/git 仍走系统 PATH）。vcpkg 经代理 7890 下载 pin 版 pwsh 后继续。

### 6.2 X12 实施记录（对照 §1）—— 已提交，验证全绿
- [x] **1.1** `SourceLocation` 增 `endLine/endColumn`（默认回退起点）。
- [x] **1.2** `Token` 增 `endLocation`。
- [x] **1.3** lexer 恢复：新增 `tokenizeRecoverable()`，把词法错误记录为 `code=2001` 诊断并跳稳定点继续扫描；原 `tokenize/scanTokens` 抛异常行为不变（兼容既有调用点）。字符串内 `;` 不切语句。
- [x] **1.4** parser 多诊断 + 子句级同步恢复：`expect/take/identifier/literal/primary` 统一走 `fail()`（strict 模式 throw 保兼容；recover 模式记录 `code=2002` 诊断 + 子句同步）。`select()` 各子句（WHERE/GROUP/HAVING/LIMIT/OFFSET）独立 try/catch，同一条语句多错误逐一报告。新增 `parseRecoverable` 返回恢复后的语句，出错语句置 `invalid=true`。
- [x] **1.5** planner 拒收：diagnostics 入口对 `invalid=true` 语句跳过编译上报，不产可执行计划、不留污染 snapshot。
- [x] **1.6** `database.cpp diagnostics()` 复用恢复式 tokenize，输出 `endLine/endColumn/statementIndex`。
- [x] **1.7** 测试扩展：`tests/diagnostics-smoke.mjs` 覆盖多词法错误逐一报告、错误后合法语句继续解析、位置/索引精确。
- [x] **1.8** 回归：parser 18 / planner 26 / subquery 24 / explain 21 / statistics 11 / diagnostics 22 / contract 全部通过。

> 本轮（builder-A 第二次提交）完成 **X12 语法层恢复（1.4）+ planner 拒收（1.5）**：Parser 引入 `fail()` 出口与 `Recovered` 信号，strict 模式行为不变、recover 模式子句级同步多诊断；`parseRecoverable` 返回恢复后语句，`invalid=true` 语句被 diagnostics 跳过编译。实现 + Parser 集成完成，后续可继续 X09/X18 等任务。

### 6.3 X13 Schema/Plan/AST 版本化实施记录（builder-A 第三次提交）

> 范围：**Phase 2（X13 Schema 迁移契约）**。分支 `builder-A`。

- [x] **2.1a** `serialization.hpp` 定义版本常量：`AST_SCHEMA_VERSION=1 / PLAN_SCHEMA_VERSION=1 / PRODUCER_VERSION=1 / CATALOG_SCHEMA_VERSION=5`（映射现状表/列描述符最新版）。
- [x] **2.1b** AST：新增 `serializeAstDocument()`（`schemaVersion+producerVersion+statements` 对称文档）与 `deserializeAst()` 未知主版本拒绝（`schemaVersion != AST_SCHEMA_VERSION` → `Storage` 错误）。`serializeAst()` 保持裸数组兼容既有调用点。
- [x] **2.1c** Plan：`deserializePlans()` 版检由硬编码 `1u` 改为 `PLAN_SCHEMA_VERSION` 常量，wrapper 文档（`schemaVersion+planKind+plans`）未知版本拒绝。
- [x] **2.2** catalog schemaVersion 落盘：新增保留关系 `CatalogMetaStore`（占用 `uint64::max`，与从 2 起、单调递增的 `int32` 用户表 id 永不相交），单行 `{schemaVersion:int, detailJson}`。迁移入口 `PersistentCatalog::migrationPlan(from)` 返回 `[from, current]` 每步 `{from,to,reversible,preflight,action,recoveryPoint}`。
- [x] **2.3a** `PersistentCatalog` 构造建档（重启校验）：absent→stamp 当前版本；`<current`→按迁移链升至 `CATALOG_SCHEMA_VERSION` 并记 `migratedFrom`；`==current`→no-op；`>current`→拒绝（`Unsupported catalog schema version N`）；非正版本/畸形 detail→`STORAGE_CORRUPTION`。
- [x] **2.3b** 迁移为**版本戳记**式：表/列/索引/约束描述符在 lenient 重读后保持字节不变，仅推进 header schemaVersion（满足“迁移不得静默改列类型/NULL/约束/索引”）；恢复点=既有单行 header，`replace` 原子提交，中断后重启沿用恢复点重跑并使库可打开。
- [x] **2.3c** 中断恢复：header detail 带 `pendingMigration>=current` 标记 → 视为崩溃中断，重启完成戳记并标 `recovered=true`。
- [x] **2.3d** `Database::catalog()` 输出新增顶层 `schemaVersion`；`PersistentCatalog::catalogMetadata()`/`catalogSchemaVersion()` 暴露版本与迁移元信息。
- [x] **2.4** 新增 `tests/catalog_migration_contract.cpp`（`minisql_catalog_migration_contract`，链接 `minisql_execution`）：迁移链 `1→current` 全部可逆/带前置/动作/恢复点；新建即戳 current；旧版 header 升级保留表；中断恢复 `recovered=true`；未知新版 `99` 拒绝且不改文件；非正版本畸形拒绝；迁移后 `execution::Database` 端到端可打开并查询。
- [x] **2.5** 回归全绿：C++ contract（heap 2698 / database 81 / optimizer 518 / planner / catalog-migration 20）+ mjs（parser 18 / planner 26 / diagnostics 22 / subquery 24 / statistics 11 / explain 21）+ database-http 联调。

**说明（X13 迁移语义）**：本系统表/列描述符自 v1–v5 均由 lenient reader 兼容读取，故“迁移”不重写描述符行，仅在成功重读后推进 catalog schemaVersion（可逆版本戳记）。这是对“迁移不得静默改列类型/NULL/约束/索引”最直接的安全实现——不会在迁移过程中改写任何用户schema。

> 后续可接 **X09（派生表/作用域/Apply/SemiJoin）** 或 **X18（统计/成本模型）**。

### 6.4 X09 实施记录（builder-A 第四次提交）

> 本轮推进 **X09 结构化子查询对象身份**。分支 `builder-A`。

- [x] **3.1a** `sql/parser.hpp`：`Expr` 新增 `subquery` 结构化节点（`std::shared_ptr<Statement>`），与过渡字段 `subquerySql` 并存；迁移到结构化对象身份后 3.3–3.5 消费该节点并移除 `subquerySql`。
- [x] **3.1b** `sql/parser.cpp`：InSubquery / Exists / ScalarSubquery 三个解析点由 `(void)select(false)` 丢弃改为保留返回的 `Statement`，赋值到 `Expr::subquery`（`select()` 本已返回结构化子查询节点）。
- [x] **3.1c** 新增 `tests/parser_subquery_contract.cpp`（`minisql_parser_subquery_contract`，链接 `minisql_sql`）：遍历 AST 校验 EXISTS/IN/标量子查询节点均携带非空 `Select` 子查询节点（含 FROM 表）且过渡 `subquerySql` 仍在。
- [x] **3.1d** 构建修复：`CMakeLists.txt` 中 `minisql_arithmetic64_probe` 在 `set(CMAKE_CXX_STANDARD 20)` 之前创建，MSVC 退化为 C++14，`<variant>`（`std::variant`）不可用；将该 probe 目标下移到标准设置之后，确保 test 目标统一 `stdcpp20`。
- [x] **3.1e** 回归全绿：C++ contract（… / parser_subquery 3 / 其余不回归）+ 全量 59 用例通过。

> 下一步 **3.2**：`sql/parser.cpp` FROM 支持派生表（显式别名、重复输出列名报歧义）、嵌套查询块；随后 **3.3** planner 以 `Scope` 链绑定替换字符串替换，生成 Apply/SemiJoin。

### 6.5 X09 Phase 3.2 记录（builder-A 第五次提交）

> 本轮推进 **FROM 派生表解析**。分支 `builder-A`。

- [x] **3.2a** `sql/parser.hpp`：`Statement` 新增 `fromSubquery`（`std::shared_ptr<Statement>`）承载 FROM 派生表子查询。
- [x] **3.2b** `sql/parser.cpp` `select()`：FROM 支持 `( SELECT ... ) [AS] alias` 派生表；必须有显式别名（缺省 → `Semantic` 错误）；输出列名重复 → `Semantic` 歧义错误（wildcard/表达式列静态无法判重则跳过）；透传 `selectList`。正常表路径不变。
- [x] **3.2c** `serialization.hpp`：`serializeStatement` 输出 `fromSubquery`（null 或嵌套 statement），`readStatement` 回读，保证 AST 往返稳定。
- [x] **3.2d** 契约测试扩展：`tests/parser_subquery_contract.cpp` 增 4 项派生表校验（子查询节点非空 / `Select` / 显式别名 / select 项保留；无别名拒绝；重复列名拒绝）。
- [x] **3.2e** 回归：C++ contract 全 59 用例通过；node parser 18 / subquery 24 / planner 26 / explain 21 通过。

> 下一步 **3.3**：planner `bindExpression()`/`build()` 建立逐查询块 `Scope` 链（当前列→当前别名→外层相关），未限定名按固定顺序解析（禁字符串替换），生成 Apply/SemiJoin。

### 6.6 X09 Phase 3.3 记录（builder-A 第六次提交）

> 本轮推进 **派生表作用域链绑定（Scope-chain）**，让 `fromSubquery` 可执行端到端。分支 `builder-A`。

- [x] **3.3a** `sql/planner.cpp` `build()`：识别派生表基座（`fromSubquery != null`）。`build(*fromSubquery)` 先递归构建内层 select 计划，以其 `output`（实体 selectItems 列）构造合成 `catalog::Table` 作为**外层作用域**（别名=派生别名）；未限定列名经 `bindColumn`/`bindExpression` 按该作用域解析，**不再字符串替换**。外层谓词/投影/ORDER BY/GROUP 统一绑定到 `bindScope`，与真实表的 `queryScope` 走同一绑定逻辑。
- [x] **3.3b** 作用域限制：DELETE/UPDATE over 派生表 → `Semantic` 拒绝；外层 JOIN over 派生表 → 显式暂不支持错误；`selectList` 兜底骨架与 `projections.empty()` 补全仅在非派生路径启用以避 `*table` 空解引用。
- [x] **3.3c** `catalog/catalog.cpp` `validate()`：派生基座语句内层 select 仍在真实 catalog 上递归校验，外层列绑定/类型/WHERE 校验交由 planner Scope 链；绕开“Table does not exist: <alias>”。
- [x] **3.3d** `execution/database.cpp` `runNode()`：外层 Select 投影于“成形子计划”（派生表）之上时先 `run()` 物化内层关系、再对外层投影求值；普通 Select（Filter 下接裸 Scan）路径不变，避免截胡。
- [x] **3.3e** 契约测试：`tests/parser_subquery_contract.cpp` 链接改 `minisql_planner`，新增 3 项 `compilePlans` 派生规划校验（根 Project/别名/**外层输出列名**/投影为派生列引用；未知外层列拒绝）。
- [x] **3.3f** 新增端到端 `tests/derived-smoke.mjs`：12 项（内层 filter+alias、外层 WHERE、`select *` 展开、qualified 引用、聚合/DISTINCT 派生表、多层派生、重复列名拒绝、缺别名拒绝、别名遮蔽原表）。
- [x] **3.3g** 回归全绿：C++ contract 全 59 用例通过；node derived 12 / subquery 24 / parser 18 / planner 26 / explain 21 / outer-join 9 / aggregate-plan 75 / update 30 / insert 45 / database 7 通过（join-process、aggregate-process 因 workbench 未装 `sql.js` 差分依赖而无法运行，属环境缺失、与本次改动无关）。

> 下一步 **3.3/3.4**：把相关子查询（EXISTS/IN/标量）从“文本重解析 + 外层列字面量替换”迁移到 by-value 参数绑定的 Apply/SemiJoin 计划节点与优化器去相关改写。

### 6.7 X09 Phase 3.5 记录（builder-A 第七次提交）

> 本轮推进 **相关子查询 by-value 参数绑定执行（非文本重解析）**。分支 `builder-A`。

- [x] **3.5a** `execution/database.cpp`：新增 `parameterLiteral()`——把外层列绑定值序列化为 SQL 字面量文本（与解析器产出的 Literal 一致，含 BOOL/浮点/整数/十进制/字符串转义/`DATE`）；新增 `bindOuter()`——递归克隆表达式树并仅把「匹配外层作用域限定名」的 `Identifier` 节点替换为携带绑定值的 `Literal` 节点；新增 `bindOuterStatement()`——克隆整条子查询 Statement 并对其 where/selectItems/orderBy/assignments/checks/valueExpressions/valueRows/groupBy/having/joins.on/fromSubquery 逐表达式绑定。
- [x] **3.5b** `database.hpp`：新增 `correlatedAstCache_`（`unordered_map<subquerySql, vector<Statement>>`）——按 subquerySql 缓存已解析的结构化 AST。
- [x] **3.5c** `runCorrelatedSubquery()`：弃用「tokenize 整段子查询文本 → 外层列字面量改写 → 重解析」路径，改为「缓存 AST → 每行对缓存做一次结构化 by-value 绑定 → 以当前 catalog 编译 → 执行」。仍保留原有外层列越界、非 SELECT、类型化字面量校验语义；缓存经当前 catalog 重新编译，schema 变更仍即时生效。
- [x] **3.5d** 回归全绿：ctest C++ contract 全 59 用例通过；node subquery 24 / derived 12 / statistics 11 / explain 21 / parser 18 / planner 26 / diagnostics 22 通过；optimizer_contract 518 项通过。
- [ ] 遗留：真正的 **Apply/SemiJoin 计划节点 + 优化器去相关**（`compiled-once` 参数化执行，进一步消除每行重编译）留待 3.4/后续阶段；本阶段已消除逐行**文本重解析** 路径。

### 6.8 X09 Phase 3.4 记录 —— 保守执行优化（builder-A 第八次提交）

> 经与用户对齐：本轮 **Phase 3.4 采用「保守执行优化」**（仅改 executor，不触 planner/optimizer/序列化），把相关子查询从「逐行重执行」优化为「按绑定参数分组、对每个不同参数物化一次（集合语义半连接）」。分支 `builder-A`。

- [x] **3.4a** `execution/database.cpp`：新增 `collectOuterReferences()`/`collectStatementOuterReferences()`——扫描子查询 AST 中实际引用到的外层列 `columnId`（去重、升序，遍历字段与 `bindOuterStatement` 对齐）。
- [x] **3.4b** `runCorrelatedSubquery()`：以 `(subquerySql|scope)` 为“相关形状”，缓存引用列；再以 `(形状|绑定值)` 分组建缓存键；命中则直接返回已物化的结果行，未命中才绑定+编译+执行并写入缓存。**结果仅取决于被引用的外层列绑定值**，故对重复参数（如多个外层行共享同一 `grp`）只执行一次，正确性等同逐行。
- [x] **3.4c** 语句级生命周期：`correlatedRowsCache_` 在 `runStatement()` 入口及 EXPLAIN ANALYZE 实际执行前清空，杜绝跨语句在数据变更后的陈旧复用。
- [x] **3.4d** `database.hpp`：新增 `correlatedColumnsCache_`（形状→引用列）与 `correlatedRowsCache_`（形状|绑定值→结果行）。
- [x] **3.4e** 新增端到端 `tests/correlated-exec-smoke.mjs`：8 项覆盖 EXISTS/IN/NOT EXISTS/标量相关的分组半连接结果正确，以及「同一库先后两条语句、后一条须反映新写入（验证语句级清缓存）」，全部通过。
- [x] **3.4f** 回归全绿：ctest C++ contract 全 59 用例通过；node correlated-exec 8 / subquery 24 / derived 12 / statistics 11 / explain 21 / parser 18 / planner 26 / diagnostics 22 通过；optimizer_contract 518 项通过。
- [ ] 遗留：真正 **Apply/SemiJoin/AntiJoin 计划节点 + 优化器去相关**（消除每行重编译的 `compiled-once` 参数化执行）是更大工程，涉及 planner 生成结构化子计划与 serialization/executor 同步改造，留待后续项目阶段（非本轮范围，经用户确认）。

### 6.9 X18 统计增量切片 —— 数值列直方图 + min/max + 生成时间（builder-A 第九次提交）

> X18 Phase 4.1（统计收集）的首个**自包含增量切片**：仅改 executor 的 `statistics()`，为数值列（int/bigint/float）收集 min/max 与等宽直方图，并给统计信息追加 `version`/`generatedAtMs`。不触 parser/planner/optimizer，可单文件验证。分支 `builder-A`。

- [x] **4.1a** `execution/database.cpp` `statistics()`：列 JSON 中对非空 distinct 集合补充 `min`（`*distinct.begin()`）与 `max`（`*distinct.rbegin()`）。
- [x] **4.1b** 数值列（`type`∈`int`/`bigint`/`float`）额外构造**等宽直方图**：`bucketCount=min(16, distinctCount)`，按区间 `[min,max]` 归一化位置分槽累计各 distinct 值的频次；`span=max-min`。空 distinct、退化为单值（`span==0`，全部落入 0 号位）、浮点边界统一处理。
- [x] **4.1c** 顶层统计 JSON 追加 `version="stats-v1-histogram"` 与 `generatedAtMs`（`system_clock` 毫秒时间戳），在不破坏既有 `scope/source/checkpointCount` 等字段前提下标记本次统计的版本与刷新时刻。
- [x] **4.1d** 新增端到端 `tests/statistics-histogram-smoke.mjs`：20 项覆盖行数/基数/null 计数/min/max、数值列直方图存在且 `1..bucketCount<=16`、直方图桶求和等于 distinct 计数、以及非数值/单值列的稳健性，全部通过。
- [x] **4.1e** 回归全绿：node statistics-histogram 20 / correlated-exec 8 / statistics 11 / subquery 24 / derived 12 / explain 21 通过；optimizer_contract 518 项、planner_contract 契约通过。
- [ ] 后续：基于本切片的 stat 元数据在 optimizer 实现**成本估算模型**（4.2：SeqScan/IndexScan/NestedLoop/HashJoin/Sort 成本公式 + 固定决胜规则 + 「同统计→同计划」确定性测试），并将 EXPLAIN 估值/成本字段并入 HTTP 契约。

### 6.10 X18 成本模型增量切片 —— 直方图驱动的范围/等值选择性（builder-A 第十次提交）

> X18 Phase 4.2 的首个增量：让 EXPLAIN 的选择率估算真正消费 4.1 的直方图，替换范围谓词一档默认 `0.33`。仍只改 executor 的选择率函数，不触 optimizer 主循环 / 既有成本公式 / HTTP 契约。分支 `builder-A`。

- [x] **4.2a** `execution/database.cpp` `selectivity()`：对**存在直方图**的数值列，按等宽直方图桶累计估算范围谓词选择率——`<`用 `fracLt`（不含界）、`<=`用 `fracLe`（含界）、`>`用 `1-fracLe`、`>=`用 `1-fracLt`；值压缩到 `[min,max]` 再定位桶，`span==0`（单值）落到 0 号桶，确定性（仅依赖直方图同源数据）。
- [x] **4.2b** 等值谓词 `=`：值超出 `[min,max]` 时返回 `0`（不再误用 `1/distinct` 高估越界选择率）；范围内的等值仍保留 `1/distinct`。
- [x] **4.2c** 缺直方图（varchar 等非数值列、缺失统计）回退原默认选择率，不崩溃；`IS NULL/IS NOT NULL` 仍用 `nullRatio`，`AND/OR/NOT` 组合仍用既有布尔折叠。
- [x] **4.2d** 新增端到端 `tests/statistics-cost-smoke.mjs`：9 项覆盖范围谓词估计行数收缩且随阈值单调、越界范围/等值归零、确定性（同 SQL 同统计重复 EXPLAIN 一致）、非数值列安全回退，全部通过。
- [x] **4.2e** 回归全绿：node statistics-cost 9 / statistics-histogram 20 / statistics 11 / explain 21 / subquery 24 / derived 12 / correlated-exec 8 通过；optimizer_contract 518、planner_contract 契约通过。
- [ ] 后续：将估计进一步落到 plan 节点的 `statsSource/estimatedRows/estimatedCost` 字段并接入 optimizer 候选比较与**固定决胜**（4.2 主体、4.3），以及成本/估值字段并入 HTTP 契约（退出条件）。

### 6.11 X18 4.3 增量——估计元数据落到 EXPLAIN plan JSON（builder-A 第十一次提交）

> 4.3 的一个**保守、冻结契约友好**切片：把 `statsSource/estimatedRows/estimatedCost` 注入 EXPLAIN 的 `plan`/`optimizedPlan` 节点 JSON，并把展示行与 plan JSON 改为**同源**（避免重复计算、保证一致）。只改 executor 的 EXPLAIN 展示路径，`serializePlans` / `compile()` 冻结契约不动。分支 `builder-A`。

- [x] **4.3a** `execution/database.cpp` EXPLAIN：`raw`/`optimizedJson` 改为可变，新增 `annotate()` 对每个 plan 节点按 id 求 `estimate()` 并写入 `estimatedRows/estimatedCost/statsSource`；`rawPlans` 与 `optimized.plans` 各自收集节点指针序（`rawNodes`/`optNodes`）分别标注，保证 id 对齐。
- [x] **4.3b** EXPLAIN 展示 `rows` 直接读已标注字段，与 plan JSON 完全同源（`estimatedRows`/`estimatedCost`/`estimateSource="stats-v1"`）。
- [x] **4.3c** 增补 `tests/statistics-cost-smoke.mjs`：断言 `optimizedPlan/Filter` 节点携带三个估计字段、与展示行估计行数相等、多次编译成本确定性；成本测试升至 12 项通过。
- [x] **4.3d** 回归全绿：node statistics-cost 12 / statistics-histogram 20 / statistics 11 / explain 21 / subquery 24 / derived 12 / correlated-exec 8 / planner-regression 26 通过；database-http（含序列化写）、database-process、bridge-regression 通过；optimizer_contract 518 通过。
- [ ] 后续/退出：把预估成本接入 optimizer**候选比较 + 固定决胜**（`hash-join` 等改为成本驱动而非无条件布尔），并将 EXPLAIN 估计字段并入 HTTP 契约/工作台展示；IndexScan 估计字段依赖与 B(X20) 第二段协商。

### 6.12 X18 4.2 主体——优化器成本驱动 join 选择（builder-A 第十二次提交）

> 4.2 的核心：优化器内部的**确定性有界成本估算** + **候选比较 + 固定决胜**，把 `hash-join` 从「无条件布尔改写」改为「成本驱动」。无运行统计时用有界默认值，因而在现有 SQL 输入上**默认行为不变（保守、无破坏）**。分支 `builder-A`。

- [x] **4.2a** `optimizer/optimizer.cpp`：新增 `optimizerRows()`（确定性，仅依计划结构）与常量 `kOptimizerDefaultRows=1000` / `kOptimizerDefaultFilterSelectivity=0.25`，覆盖 SeqScan/Filter/Limit/Distinct/Aggregate/Join/Sort/Project 的行数估算，作为「缺失统计的有界默认值」。
- [x] **4.2b** hash-join 规则改造：`NestedLoopJoin` 满足等值键后，比较 `HashCost=L+R` 与 `NL Cost=L*M`；**只当 `L+R <= L*M` 才改写为 HashJoin**（相等时固定偏好 HashJoin，决胜不依赖容器序/随机值）。多行两端→HashJoin（与既有 518 契约保持一致）；估算行数≤1 的一侧→保留 NestedLoopJoin（新行为）。
- [x] **4.2c** 新增端到端 `tests/statistics-costjoin-smoke.mjs`：6 项验证多行等值连接选 HashJoin 且替换掉 NestedLoop、同 SQL 多次优化 join 选择序列一致（确定性）、HashJoin 路径返回正确行数；并留注「当前 parser 不支持派生表+JOIN、优化器对表按默认行数估算，SQL 层暂难构造单行端→NL 分支，留待派生表 JOIN/真实统计接入后验证」。
- [x] **4.2d** 回归全绿：optimizer_contract 518、planner_contract 契约通过；node statistics-cost 12 / statistics-costjoin 6 / explain 21 / statistics 11 / statistics-histogram 20 / subquery 24 / derived 12 / correlated-exec 8 / index 29 通过；database-http、bridge 集成 7 通过。
- [ ] 后续/退出：接入真实统计后让 `optimizerRows` 消费 `statistics()`/直方图（不再统一默认 1000），使单行/低基数场景真正影响 join 选择；把成本/估计并入 HTTP 契约与工作台展示；IndexScan 估计字段依赖与 B(X20) 第二段协商。

### 6.13 X18 4.2 闭环——优化器消费真实表行数统计（builder-A 第十三次提交）

> 让 6.12 的成本模型**真实生效**：把表行数统计注入优化器，替代「统一默认 1000」。由此单行端等值连接在真实统计下正确选择 NestedLoopJoin，多行两端选 HashJoin——成本驱动从「形同虚设」变为「实际决策」。分支 `builder-A`。

- [x] **4.2e** `optimizer.hpp` `Options` 末尾新增 `std::function<std::optional<double>(const std::string&)> tableRows`（放在结构末尾，保持既有 `{true,false,false}` 聚合初始化兼容）；`optimizer.cpp` `optimizerRows()` 改为接收 `Options` 并对 `SeqScan` 优先 `options.tableRows(name)`，缺表/未提供回退 `kOptimizerDefaultRows`。
- [x] **4.2f** `database.hpp/.cpp`：新增 const 私有 `estimatedTableRows(name)`（真实 `heap_.scan` 计数）；`heap_` 标 `mutable`（允许 const `compile()` 只读路径暖缓存估算）；三处 `optimizer::optimize()` 调用（compile / EXPLAIN / execute）统一注入 `optimizerOptions.tableRows=[this]...`，让优化/执行/解释三条路径共享同一成本输入。
- [x] **4.2g** 更新 `tests/statistics-costjoin-smoke.mjs`：真实统计 `t`(10 行) 与 `s`(1 行)——多行两端→HashJoin、单行端(`L=10,R=1`,Hash 11>NL 10)→保留 NestedLoopJoin、双路径确定性、结果行数正确；升至 10 项通过。
- [x] **4.2h** 回归全绿：optimizer_contract 518、planner_contract 契约、database-http（含序列化写）通过；node statistics-costjoin 10 / statistics-cost 12 / explain 21 / statistics 11 / statistics-histogram 20 / subquery 24 / derived 12 / correlated-exec 8 / index / self-foreign-key 63 通过。
- [ ] 后续/退出：进一步让 `optimizerRows` 消费列级直方图/选择率做连乘与交之决策（当前仅用表行数），并把成本/估计并入 HTTP 契约与工作台展示；IndexScan 估计字段依赖与 B(X20) 第二段协商。

### 6.14 X18 4.2-iv——优化器消费列级直方图选择率（builder-A 第十四次提交）

> 让 6.13 的 `optimizerRows` 从「仅用表行数 + 统一 0.25 过滤选择率」升级为**消费列级直方图**：旁路下推后的 Filter 按谓词（AND/OR/NOT 连乘与交、范围直方图桶、等值越界归零）做列级选择率，真实改变 join 的 Hash/NL 成本选择。分支 `builder-A`。

- [x] **4.2iv-a** `optimizer.hpp` `Options` 末尾新增 `std::function<double(const nlohmann::json&, const std::string&)> selectivity`（Filter 估计回调，(predicate, tableName)→[0,1]，同 `database.cpp` 的 `columnSelectivity`）。`optimizer.cpp` `optimizerRows()` 的 Filter 分支改为 `rows * (options.selectivity ? options.selectivity(...) : 0.25)`。
- [x] **4.2iv-b** 旁路下推后 join 选择不再锁死：在 `pushPredicateIntoJoin` 触发后，按已下推的过滤子输入**双向重判** Hash/NL（`L+R<=L*R→Hash`，否则→NL；Hash(11)>NL(10) 时由先前原始行数判定的 HashJoin 撤销回 NestedLoopJoin）——过滤序列（原始 → 下推）跨迭代收敛、不振荡。
- [x] **4.2iv-c** `database.cpp`：把原有 SELECT 过滤选择率逻辑抽为共享自由函数 `columnSelectivity(byTable, predicate, table)`（匿名命名空间）；EXPLAIN 原内联选择性闭包改用之（estimate() 与优化器 `Options.selectivity` 同源去重）。新增 const 私有 `tableStats()`（单遍扫描各表列统计含直方图）；`statistics()` 复用其输出（表序/字段不变）。compile/execute 经惰性 λ 注入 `selectivity`（遇 Filter 才扫一次列统计）。
- [x] **4.2iv-d** 新增端到端 `tests/statistics-optimizer-selectivity-smoke.mjs`：14 项覆盖——`x.a<1`（直方图选择率 0.1，左 1 行）旁路过滤器→保留 NestedLoopJoin（无直方图则 0.25→HashJoin）；`x.a<11`（选择率 1.0）→HashJoin；连乘 `x.a<1 AND x.a>=0`→NestedLoopJoin；确定性；旁路下推 Filter 节点 `estimatedRows≈1`、`statsSource`；结果行数正确。
- [x] **4.2iv-e** 回归全绿：ctest 59、optimizer_contract 518、planner_contract 契约、database_contract 81 通过；node statistics-optimizer-selectivity 14 / statistics-costjoin 10 / statistics-cost 12 / explain 21 / statistics 11 / statistics-histogram 20 / subquery 24 / derived 12 / correlated-exec 8 / parser 18 / planner 26 / diagnostics 22 通过。
- [x] **4.2iv-f** 后续跟进（本节相关）：把成本/估计并入 HTTP 契约（见 6.15）。

### 6.15 X18 4.3——成本/估计并入 HTTP 契约与工作台展示（builder-A 第十五次提交）
> 让统计驱动的成本/估值走出「仅 EXPLAIN 展示」，落入普通查询的 HTTP 契约：`compile()` 顶层 `plan` / `optimizedPlan` 每个节点带 `estimatedRows` / `estimatedCost` / `statsSource`，顶层加 `estimateModel` / `estimatedRowsAvailable`；工作台计划视图渲染时以徽标展示估值。分支 `builder-A`。

- [x] **4.3-a** `database.hpp` 声明私有 `annotatePlanEstimates(serialized, plans)`；`database.cpp` 实现——DFS 收集节点（顺序与 `serializePlans` 的 id 一致，id 即 nodes 下标），行数用 `estimatedTableRows`（与优化器同源），过滤选择率用惰性 `columnSelectivity`（仅遇 Filter 扫一次 `tableStats()`），按计划树递归估算 `estimatedRows` / `estimatedCost`（SeqScan、Filter、Sort、Limit、Distinct、Aggregate、NestedLoop/Left/Right/Full/HashJoin），并打 `statsSource="stats-v1"`。
- [x] **4.3-b** `compile()` 返回体改为 `annotatePlanEstimates(serializePlans(...))` 分别作用于 `plan` 与 `optimizedPlan`，顶层新增 `estimateModel="stats-v1"`、`estimatedRowsAvailable=true`；保留全部既有字段（kind/detail/id……）不被覆盖。EXPLAIN 结果（execute 路径）继续携带同源字段，口径一致。
- [x] **4.3-c** 工作台：`types.ts` `PlanRow` 增加可选 `estimatedRows` / `estimatedCost` / `statsSource`；`CompilerViews.tsx` 计划行在存在估计时渲染右对齐徽标 `估 <rows> 行 · 代价 <cost>`（title 展开三字段），辅助 `estimatedMetaAvailable` / `fmtEstimate`（k/M 缩写）；`styles.css` 新增 `.plan-estimate`（含 dark-mode）。
- [x] **4.3-d** 新增端到端 `tests/statistics-compile-estimate-smoke.mjs`：15 项覆盖——顶层 plan/optimizedPlan 每节点带三字段、`estimateModel`/`estimatedRowsAvailable`、SeqScan 估行=全表、Filter 直方图选择率生效、id 渐增 DFS 序不破坏、既有字段保留、跨次确定性、EXPLAIN 结果节点仍带字段。并加入 `run-minisql-tests.ps1` compiler 组。
- [x] **4.3-e** 回归全绿：planner_contract / optimizer_contract 518 / database_contract 81 / compile 契约通过；node compiler 组（新增 15 项统计契约）与 http 组（database/session/multi-session/access-control/observability/cancel/result-budget/x25-stream）通过；前端 `tsc -b && vite build` 通过。
- [x] **4.3-iv** 解锁 IndexScan 估计：新增 `indexScanSelectivity`（合成带 columnId 的列谓词逐列连乘，等值 `1/distinct` 越界归零、范围走直方图，回退 0.25），`annotatePlanEstimates` 与 EXPLAIN `estimate()` 的 IndexScan 分支用它给 `estimatedRows/estimatedCost`；新增 `statistics-index-estimate-smoke.mjs`（13 项）。回归：compiler 组 + http 组全绿。

### 6.16 X18 收尾核对——hash-join 候选比较固定决胜（builder-A 第十六次提交）
> 复核确认：`hash-join` 规则**并非**布尔开关，已是**成本驱动候选比较 + 固定决胜**——`optimizerRows`（真实行数/列级选择率）下 `L+R<=L*R→HashJoin`（相等时固定偏好 HashJoin，确定性），4.2-iv 进一步在旁路下推后双向重判（过滤收窄可撤销回 NestedLoopJoin）。`statistics-costjoin-smoke.mjs` 已断言成本驱动选择与跨编译确定性。故 X18 成本/估计主线收尾：SeqScan/IndexScan/join 估算进 HTTP 契约、工作台徽标展示、IndexScan 列级选择率均落地。

### 6.17 X09 4.x——相关子查询去相关（SemiJoin/AntiSemiJoin）（builder-A 第十七次提交）
> 把相关 `EXISTS / IN / NOT EXISTS` 从「每绑定值文本重编译」改为 **compiled-once 的结构化子计划 + 参数化执行**：planner 将 WHERE 顶层 AND 中可提升的相关子查询改写为 `SemiJoin / AntiSemiJoin` 逻辑节点，子计划外层列以 `Parameter` 节点表达，执行器按 `paramBinding` 逐左行绑定后 in-process 运行子计划。分支 `builder-A`。

- [x] **4.x-a** `planner.hpp` `LogicalPlan` 新增 `paramBinding`（数组 `{paramId, columnId, type}`）；`planner.cpp` 引入 `CorrelatedScope`（thread_local + RAII 守卫，不侵入 bindExpression 签名），`bindExpression` 命中活动关联作用域的「外层别名.列」时生成 `{"kind":"Parameter","paramId",...}` 节点；`serializePlans/deserializePlans` 透传 `paramBinding`，`validateExpression` 新增 `Parameter` 白名单。
- [x] **4.x-b** `planner.cpp` 新增 `decorrelateWhere(input, predicate, catalog)`：顶层 AND 拆分 → 识别可提升的相关 `Exists→SemiJoin`、`NOT EXISTS→AntiSemiJoin`、`IN→SemiJoin(+residual)`（残差谓词把 IN 左操作数等值判定附加到右侧过滤）；`buildRight` 一次性（compiled-once）重解析并编译子计划为右 child，外层列注入关联作用域；单表/非聚合 SELECT 才触发（`canDecorate`），其余路径保持原 Filter 折叠。
- [x] **4.x-c** `optimizer.cpp`：`rewrite` 对 `Parameter` 透传；`optimizerRows` 为 `SemiJoin/AntiSemiJoin/Apply` 增加行数估计（Apply≈left*right、SemiJoin≈left*selectivity 复用 `Options.selectivity`、AntiSemiJoin≈left*0.5），供 EXPLAIN/成本展示。
- [x] **4.x-d** `database.hpp` 新增 `correlationParam(paramId)` + 成员 `correlationParams_`；`database.cpp`：`evaluate` 增加 `Parameter` 分支读参数环境；`joinRows` 增加 `SemiJoin/AntiSemiJoin/Apply` 执行——按 `paramBinding` 从每左行取值绑定参数（`bindCorrelation`），in-process 运行右子计划（不再逐值重新 compile/run），SemiJoin/AntiSemiJoin 保留/过滤 `matched` 左行（SemiJoin 残差谓词在右行上求值），Apply 处理标量多行报错/空集补 NULL；`runStatement` 每次语句复位 `correlationParams_`。
- [x] **4.x-e** 修复 `WHERE NULL` 回归：`trivial` 判断用 `contains("value") && at("value").is_boolean()` 替代 `value("value", false)`（原实现对 `value==null` 触发 `get<bool>` type_error 302，导致 optimizer_contract 挂起）。修复后 ctest 59/59、optimizer_contract 518 全绿。
- [x] **4.x-f** 回归：ctest 59/59；node correlated-exec 8 / subquery 24 / derived 12 / explain 21 / outer-join 9 / parser 18 / planner 26 / diagnostics 22 通过；`SELECT ... WHERE EXISTS(...)` 的 EXPLAIN 顶层出现 `SemiJoin` 节点与 `paramBinding`，执行结果与集合语义一致（IN/NOT EXISTS/标量）。（注：`in-list-smoke`/`join-process` 的「拒绝 IN 子查询 / JOIN 无 ON」断言为 HEAD 已存在的基线失败，与本次去相关改动无关。）
- [x] **4.x-g** 相关**NOT IN→AntiSemiJoin(+residual)** 去相关：`decorrelateWhere` 识别 `Unary NOT over InSubquery`（`ifNotIn`），`buildRight` 一次性编译右子计划，残差把 IN 左操作数等值判定附加右侧过滤；执行器 `joinRows` 的 `AntiSemiJoin` 在有残差时改为「右行值 NULL 或残差成立 判命中」→ 排除（SQL：子查询含 NULL 时 NOT IN 为 NULL）。EXPLAIN 顶层出现 `AntiSemiJoin` + `paramBinding` + 残差谓词。
- [x] **4.x-h** 相关**标量子查询→Apply(scalar)** 去相关：`decorrelateWhere` 识别纯布尔标量（`ifScalarBool`）与比较型标量（`ifScalarComp`，子查询在 Binary left 或 right），生成 `Apply` 节点（`values={"scalar":true}`），残差把子查询一侧替换为追加标量列 Identifier 并保持操作数位置；执行器 `Apply` 标量分支求值残差谓词过滤（空集补 NULL→比较 NULL→排除、多行报 `ExecutionError`）。EXPLAIN 顶层出现 `Apply` + `values.scalar` + `paramBinding` + 残差谓词。
- [x] **4.x-i** 回归：新增 `tests/decorrelate-apply-smoke.mjs`（12 checks：NOT IN / IN / NOT EXISTS / EXISTS / 标量等值 / 标量左比较 / 空集标量 / 派生表基座 IN & NOT IN & 标量）。node parser 18 / planner 26 / diagnostics 22 / subquery 24 / explain 21 / outer-join 9 / statistics 11 / histogram 20 / cost 12 / costjoin 10 / optimizer-selectivity 14 / correlated-exec 8 / derived 12 / decorrelate-apply 12 通过；C++ contracts：optimizer 518 / planner / parser_subquery 3 / database 全绿。
- [x] **4.x-j** 收口 `ScalarSubquery` 在 **HAVING / 聚合投影 / ORDER BY** 位置的相关求值，并修正「子计划含聚合时去相关产出不可执行节点」的缺口（详见 §6.19）。
- [ ] 后续（非正确性，属重解析/缓存优化）：右子计划以 `Expr::subquery` 结构化 AST 优先替代文本重解析；跨库缓存与 `correlatedRowsCache_` 结合做绑定级 memo。

### 6.18 基线漂移核对与修复（builder-A 第十八次提交）

> 在 `builder-A` HEAD `7d44676` 上继续收敛：先修两处真实缺陷，再核对全量回归的既存失败（对照 progress 文档区分「产品缺陷」与「测试契约滞后」）。提交 `240acee`（author/committer=anyu999），已推送 `7d44676..240acee`。

- [x] **6.18-a 修复解析器栈溢出**：`sql/parser.cpp` 表达式深度上限由 `256` 改 `kMaxExpressionDepth=128`。递归下降表达式链约 8 帧/层，默认 1 MiB 线程栈约在 190 层耗尽，原上限永不触发即 `0xC00000FD` 崩溃；同时 `allRecoverable()` 每条语句重置 `depth`，避免恢复回绕把后续语句误判。深层 `CAST(`/`(`/`SUM(` 嵌套现返回 `2002 Expression depth exceeded`。
- [x] **6.18-b 修复 DATE 字面量误判**：`sql/planner.cpp` `bindExpression` 中 `DATE '...'` 的 `E` 命中指数启发式被标为 `float`（值仍是字符串），`WHERE d < DATE '2000-01-01'` 报内部类型错误；改为先判 `dateLiteralText`，命中后跳过 float/decimal 启发式。日期比较恢复正常。
- [x] **6.18-c 全量回归基线**：ctest **59/59**；node 修复前 **70/76**，核对后确认 6 项失败均为**测试契约滞后或环境**，逐条更正：
  - `in-list-smoke`：`id IN(SELECT …)` 已由 X09 半连接支持 → 断言改为校验过滤结果 `[[1],[2]]`。
  - `join-process`：`RIGHT/FULL JOIN` 已实现且由 `outer-join-smoke.mjs` 正向覆盖 → 从「拒绝」清单移除；等值连接在 `optimizedPlan` 中已按 EXT-OPT-003 改写为 `HashJoin` → 断言改查 `HashJoin.predicate.operator`。
  - `insert-columns-process`：`INSERT INTO t(id) VALUES(4),(5)` 多行+显式列清单已由 multirow-progress 启用 → 从「拒绝」清单移除。
  - `decimal-literal-process`：指数形式已是合法 FLOAT 字面量（见 float-process 与 V3 技术文档 §3.2）→ 移出 DECIMAL 畸形清单，并正向断言 `1.2e3 → 1200`。
  - `write-batch-process`：`bin/write_batch_contract.exe` 缺名（MSVC 产物带 `minisql_` 前缀，`bin/` 约定为去前缀名）→ 补齐该产物。
  - `journal-process`：全量连跑时触发本地批量删除护栏（产物累计 1067，>50/回合）；清空 `tests/artifacts` 后单独运行 **88 项通过**，非产品失败。
- [x] **6.18-d 复核**：ctest 59/59；node **76/76**（`journal-process` 于干净产物目录单独复跑通过）。

### 6.19 X09 4.x 收口——相关子查询在 HAVING / 聚合投影 / ORDER BY 位置

> 收口 §6.17 的遗留项。定位到两处**真实缺口**（非文档口径问题），逐条用对抗性用例验证。分支 `builder-A`。

- [x] **6.19-a 缺口定位**：对 `ScalarSubquery` 五个位置实测——投影与 ORDER BY **本已正确**（执行期按行绑定 `runCorrelatedSubquery`，早前一次误判是测试数据单调所致）；真正的缺口是
  - `HAVING (SELECT COUNT(*) … WHERE u.k = t.grp) > 0` → `5001 Correlated subquery outer column outside row`；
  - `WHERE id = (SELECT MAX(u.id) … WHERE u.id = t.id)` → `5001 Unsupported join input`。
- [x] **6.19-b 根因一（HAVING）**：`lowerAggregate` 的 `rewrite` 只重写分组键 / `AggregateExpr`，而相关子查询的外层列引用存放在独立字段 `outerColumns`（`{限定名: {columnId, type}}`）中，未被重写。聚合之后实际求值的行是**聚合输出行**（分组键 + 聚合槽位），外层列却仍带**基表列下标** → 越界或取自错误列。修复：`lowerAggregate` 新增 `remapGroupRefs`，把 HAVING / 聚合投影中相关子查询的 `outerColumns.columnId` 重映射到 `GROUP BY` 键在 `aggregate.output` 中的槽位；引用**未分组列**时按 SQL 语义报 `2003 Column must be grouped or aggregated`（分组键本身是表达式时同样拒绝，避免基表下标在聚合行上取错列）。
- [x] **6.19-c 根因二（WHERE + 聚合子计划）**：`decorrelateWhere` 原先无条件把可提升的相关子查询改写为 `SemiJoin/AntiSemiJoin/Apply`，但执行器的 in-process `joinRows` 只支持 `Project/Filter/Scan/Join`；子计划含 `Aggregate`（或 `Sort/Limit/Distinct`）时必然落到 `Unsupported join input`。修复：新增 `inProcessExecutable(subplan)` 判定，子计划含 `joinRows` 不支持的节点时**不去相关**，退回执行期按行绑定（`Correlated*` 表达式）路径——语义一致，且该路径已在投影位置长期验证，避免生成运行期必然失败的 `Apply/SemiJoin`。
- [x] **6.19-d 行为核对**（逐条实跑，非仅翻转断言）：HAVING 相关标量 / `EXISTS` / `NOT EXISTS` / `IN`、HAVING 两侧同时含聚合的比较、表别名限定名；聚合投影 `SELECT grp, (SELECT COUNT(*) …)`；WHERE 侧 `=` / `IN` / `NOT IN` / `COUNT(*)` 聚合。其中 `NOT IN` 命中 SQL 的经典陷阱——子查询为 `MAX(空集)` 时返回**单行 NULL**，`NOT IN {NULL}` 为 NULL → 整行排除（结果为空集）。
- [x] **6.19-e 结构化断言**：简单子计划仍生成 `SemiJoin`（去相关生效）；含 `MAX` 的子计划回退为 `Filter`（去相关关闭）；HAVING 计划为 `Filter → Aggregate → SeqScan`。
- [x] **6.19-f 回归**：新增 `tests/correlated-aggregate-smoke.mjs`（**21 checks**）。ctest **59/59**；node **77/77**（`journal-process` 与 §6.18 同因：全量连跑会触本地批量删除护栏，清空 `tests/artifacts` 后单独运行 **88 项通过**，非产品失败）。

### 6.20 X13 版本契约补全——nodeVersion/planVersion + 同主版小版本兼容读

> 收口任务书 A2 范围 1/2。定位到两处**真实缺口**：① AST/Plan 文档只有 `schemaVersion` + `producerVersion`，缺任务书要求的 `nodeVersion`/`planVersion`；② 读端版本比对是**裸值精确相等**（`serialization.hpp` 的 `document.at("schemaVersion") != AST_SCHEMA_VERSION`、`planner.cpp` 的 `document.value("schemaVersion", 0u) == PLAN_SCHEMA_VERSION`），既无「未知主版本拒绝」的显式语义，也无「同主版小版本兼容读」。分支 `builder-A`。

- [x] **6.20-a 常量与读端闸门**：`serialization.hpp` 新增 `AST_SCHEMA_MINOR / AST_NODE_VERSION / PLAN_SCHEMA_MINOR / PLAN_VERSION`，并引入统一闸门 `versionReadable(documentMajor, documentMinor, currentMajor, currentMinor)`（主版本必须相等、次版本不得高于本二进制）与 `readVersionField`（接受 JSON 有符号/无符号整数，拒绝缺失/错型/负值/越界）、`optionalVersionReadable`（缺省字段视为最旧可读形态）。
- [x] **6.20-b AST 文档**：`serializeAstDocument` 写入 `schemaVersion/schemaMinor/nodeVersion/producerVersion`；`deserializeAst` 改用版本闸门——未知主版本（如 `schemaVersion:2`）拒绝，`schemaMinor` 高于当前（1 > 0）拒绝，`nodeVersion` 未知主版本拒绝，缺省次版本按 0 兼容读。
- [x] **6.20-c Plan 文档**：新增 `serializePlanDocument(plans)`（writer，输出 `schemaVersion/schemaMinor/planVersion/producerVersion/planKind/plans`，与 `serializePlans` 往返一致）；`deserializePlans` 改用同一版本闸门。**不改动 compile 响应既有结构**（`database.cpp`/`compile_main.cpp` 的 `schemaVersion:1`）——按任务书 8.1 接口冻结，响应接入（含前端 `types.ts` 同步）留待跨组同步，本提交只完成编解码契约。
- [x] **6.20-d 回归**：`tests/planner_contract.cpp` 新增 12 项断言（版本化往返、主版拒绝、更高次版拒绝、`nodeVersion`/`planVersion` 未知主版拒绝、同主版次版兼容读、`serializeAstDocument`/`serializePlanDocument` 字段存在）；`ctest` **59/59**；node `planner-regression` 26 项通过。

### 6.21 X18 显式 ANALYZE——统计刷新入口与刷新记录

> 收口任务书 A4 范围 2（"统计刷新使用显式 `ANALYZE` 或受控后台任务，记录刷新时间和统计版本"）。此前 `statistics()` 恒为 `source=on-demand-scan`，无显式刷新入口，也无刷新时间/版本的持久记录。分支 `builder-A`。

- [x] **6.21-a 语法与执行**：`Database::execute` 在语句分派处识别 `ANALYZE [TABLE] <name>`（与 `EXPLAIN` 同为分派级特判，**不改 AST/Plan 契约**）。校验表存在（`catalog_.view().find`），命中后单遍 `tableStats()` 刷新全库表统计。
- [x] **6.21-b 刷新记录持久化**：把 `{table, analyzedAtMs, version:"stats-v1-histogram", tables}` 写入数据库旁的 `<db>.analyze.json`。`statistics()` 改为优先读该快照，输出 `source:"analyze"`、`generatedAtMs`/`lastAnalyzeAtMs` 为刷新时间；缺失时回退实时扫描（`source:"on-demand-scan"`、`lastAnalyzeAtMs=0`）。
- [x] **6.21-c 失效规则**：`runStatement` 对任何写语句（CreateTable/CreateIndex/DropIndex/Insert/Update/Delete）**成功后删除**旁路文件——ANALYZE 快照随即失效，`statistics()` 回退实时扫描。
- [x] **6.21-d ANALYZE 响应**：`kind=Analyze`，`columns=[table,rowCount,columnCount,analyzedAtMs,statsVersion]`，行仅含目标表，附 `source/statsVersion/analyzedAtMs`；EXPLAIN 展示路径的统计消费保持同源。
- [x] **6.21-e 回归**：新增 `tests/analyze-stats-smoke.mjs`（**35 checks**：初态 on-demand、ANALYZE 后 `source=analyze` + 时间戳/版本、`ANALYZE TABLE` 与关键字大小写、写语句后失效、未知表/缺表名/多余 token 三类语法错误、EXPLAIN 不受影响）。`ctest` **59/59**。
