# X09 子查询与作用域 —— progress

> 成员 A 交付物（任务书 A5-①）。设计目标见任务书 A3；执行记录见 `task-A-development-plan.md` §6.4–§6.8、§6.17、§6.19。

## 状态：正确性已完成；结构化收口为可选优化

## 已实现

- **派生表**：FROM 项支持派生表（显式别名；重复输出列名报歧义）与嵌套查询块。
- **作用域链（单层）**：逐查询块 `Scope`，查找顺序为当前块列 → 当前别名 → 外层相关；未限定名按固定规则解析，禁止字符串替换实现相关引用。
  - ⚠️ **已知缺口**：仅支持**单层**外层相关。`catalog.cpp` 的语义分析与 `planner.cpp` 的 `correlatedScope(table)` 都只维护单层 `scope`，`outerColumns` 仅取紧邻外层表的列；**两级及以上**嵌套（如 `... WHERE EXISTS (SELECT ... WHERE EXISTS (SELECT ... WHERE z.id = x.id))`）报 `2003 Unknown table qualifier: x`。任务书 A3 设计范围 2/3 与验证项（"派生表和**三层作用域测试**"）要求多层作用域链，**尚未实现**（详见 `task-A-development-plan.md` §6.23-d / §7.1-C）。
- **结构化子查询节点**：`Expr` 同时携带 `subquerySql`（文本）与 `subquery`（结构化 `Statement`）；parser 在 IN / EXISTS / 标量位置填充结构化节点。
- **Apply / SemiJoin / AntiSemiJoin**：`decorrelateWhere` 把 WHERE 顶层 AND 中可提升的相关子查询改写为这些节点；子计划外层列以 `Parameter` 表达，执行器按 `paramBinding` 逐左行绑定（compiled-once，不再逐值重新 compile/run）。
- **保守边界**：子查询含聚合 / DISTINCT / LIMIT，或右子计划含 `joinRows` 不支持的节点（Aggregate / Sort / Limit / Distinct）时**不去相关**，回退执行期按行绑定（`Correlated*` 表达式）路径。
- **HAVING / 聚合投影 / ORDER BY**：`lowerAggregate` 的 `remapGroupRefs` 把相关子查询 `outerColumns.columnId` 重映射到 GROUP BY 键在 `aggregate.output` 的槽位；引用未分组列报 `2003`。
- **NULL 三值逻辑**：IN / NOT IN / EXISTS / 标量在空集与 NULL 下与 SQL 语义一致（`NOT IN {NULL}` → NULL → 整行排除）。

## 验证

- `subquery-smoke` 24 / `derived` 12 / `correlated-exec` 8 / `decorrelate-apply` 12 / `correlated-aggregate` 21 / `outer-join` 9 项通过；与 SQLite 差分 7 例值一致（3 处差异仅为默认 NULL 排序约定不同）。

## 未完成

- **【功能缺口·未做】多层（≥2 级）嵌套作用域链**：见上「作用域链（单层）」的已知缺口与 §6.23-d。这是**唯一已知的真实功能缺口**（相对任务书 A3 设计范围 2/3），需同时改语义分析（scope 链）、planner（`outerColumns` 合并 / `activeCorrelated` 链式）与执行期绑定（多级绑定值），风险覆盖 X09 全量，须单独排期决策。
- **[非正确性优化·暂缓] 子查询结构化收口**：planner 中间表示为 JSON，predicate 仅携带 `subquerySql` 文本，因此 `remapGroupRefs` / `isCorrelated` / `buildRight` 仍以 `tokenize + parse` 重解析子查询文本。改为"结构化 AST 优先"需把结构化子查询贯穿 planner 中间表示（或引入编译期子查询注册表），涉及计划 JSON 结构取舍，待单独评估。
- **[已完成] 绑定级 memo**：结果行缓存已由「每语句清空」改为「数据/目录版本驱动失效」，在长驻 session 进程内跨语句复用；`statistics().correlatedMemo` 暴露 hits/misses/entries。见 §6.22、`tests/correlated-memo-smoke.mjs`（17 checks）。
