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
- [x] **3.3g** 回归全绿：C++ contract 全 59 用例通过；node derived 12 / subquery 24 / parser 18 / planner 26 / explain 21 / outer-join 9 / aggregate-plan 75 / update 30 / insert 45 / database 7 通过；join-process、aggregate-process、null-process 已迁移到 Node 24 `node:sqlite`，不再依赖已删除的 `sql.js`/Demo Worker。

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
