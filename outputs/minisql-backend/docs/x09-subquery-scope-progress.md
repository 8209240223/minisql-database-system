# X09 子查询与作用域 —— progress

> 成员 A 交付物（任务书 A5-①）。设计目标见任务书 A3；执行记录见 `task-A-development-plan.md` §6.4–§6.8、§6.17、§6.19。

## 状态：正确性已完成；结构化收口为可选优化

## 已实现

- **派生表**：FROM 项支持派生表（显式别名；重复输出列名报歧义）与嵌套查询块。
- **多层作用域链**：逐查询块 `Scope` 使用父级链和稳定 `BindingId`，查找顺序为当前块列 → 当前别名 → 各级祖先相关列；未限定名按固定规则解析，禁止字符串替换实现相关引用。
  - Planner 将当前查询块可见的祖先 binding 写入相关参数；执行器维护祖先参数作用域栈，递归物化子查询时同时传递父级和祖父级绑定。
  - 相关结果缓存键包含完整祖先绑定值，避免不同外层组合错误复用同一物化结果。
- **结构化子查询节点**：`Expr` 同时携带 `subquerySql`（文本）与 `subquery`（结构化 `Statement`）；parser 在 IN / EXISTS / 标量位置填充结构化节点。
- **Apply / SemiJoin / AntiSemiJoin**：`decorrelateWhere` 把 WHERE 顶层 AND 中可提升的相关子查询改写为这些节点；子计划外层列以 `Parameter` 表达，执行器按 `paramBinding` 逐左行绑定（compiled-once，不再逐值重新 compile/run）。
- **保守边界**：子查询含聚合 / DISTINCT / LIMIT，或右子计划含 `joinRows` 不支持的节点（Aggregate / Sort / Limit / Distinct）时**不去相关**，回退执行期按行绑定（`Correlated*` 表达式）路径。
- **HAVING / 聚合投影 / ORDER BY**：`lowerAggregate` 的 `remapGroupRefs` 把相关子查询 `outerColumns.columnId` 重映射到 GROUP BY 键在 `aggregate.output` 的槽位；引用未分组列报 `2003`。
- **NULL 三值逻辑**：IN / NOT IN / EXISTS / 标量在空集与 NULL 下与 SQL 语义一致（`NOT IN {NULL}` → NULL → 整行排除）。

## 验证

- `subquery-smoke` 24 / `derived` 12 / `correlated-exec` 8 / `decorrelate-apply` 12 / `correlated-aggregate` 21 / `outer-join` 9 项通过；与 SQLite 差分 7 例值一致（3 处差异仅为默认 NULL 排序约定不同）。
- `minisql_database_contract` 89 项通过，其中包含三层父级+祖父级引用、仅祖父级引用，以及祖先绑定缓存隔离。

## 未完成

- **[已完成] 多层（≥2 级）嵌套作用域链**：Binder/Planner/执行器和缓存键均已支持祖先作用域，三层查询已进入 C++ database contract。
- **[非正确性优化·暂缓] 子查询结构化收口**：planner 中间表示为 JSON，predicate 仅携带 `subquerySql` 文本，因此 `remapGroupRefs` / `isCorrelated` / `buildRight` 仍以 `tokenize + parse` 重解析子查询文本。改为"结构化 AST 优先"需把结构化子查询贯穿 planner 中间表示（或引入编译期子查询注册表），涉及计划 JSON 结构取舍，待单独评估。
- **[已完成] 绑定级 memo**：结果行缓存已由「每语句清空」改为「数据/目录版本驱动失效」，在长驻 session 进程内跨语句复用；`statistics().correlatedMemo` 暴露 hits/misses/entries。见 §6.22、`tests/correlated-memo-smoke.mjs`（17 checks）。
