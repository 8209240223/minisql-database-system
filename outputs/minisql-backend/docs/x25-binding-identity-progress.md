# X25 稳定绑定标识与作用域树

## 目标

名称解析只发生一次，发生在绑定器里。优化器、序列化、权限检查都消费同一份
`BindResult`，不再各自重新解释 SQL 文本、表名字符串或列名下标。

改造前，「这条 SQL 访问了哪些对象」这个问题在三处分别被回答：

| 位置 | 手段 | 问题 |
| --- | --- | --- |
| `security/access_catalog.cpp` `tableReferences` | 跳过注释/字符串的词法扫描 | 关键字启发式；CTE 别名靠括号配平排除 |
| `security/access_catalog.cpp` `astTableReferences` | 解析 AST 再走一遍访问者 | 与 `Database` 里的访问者重复；解析失败即回退扫描 |
| `execution/database.cpp` `resolveAccessObjects` + `lexicalAccessObjects` | AST 访问者 + 第二份词法扫描 | 同上，且 `security` 因此反向依赖 `sql` 层 |

三份实现的对象集合可以不一致，而不一致的方向是**漏授权**：词法扫描认不出的
来源不会被检查。X25 把它们合并成一个绑定器，并把回退路径整体删除。

## 本轮实现（阶段一：标识层 + 鉴权线）

### 稳定标识

`include/minisql/sql/binding.hpp` 定义四个强类型标识，均为 `enum class :
uint32_t`，`0` 恒为 `Invalid`，在一次绑定内稠密且从 1 开始：

- `RelationId` —— 一个 FROM 项。别名不产生独立实体；同一物理表被引用两次得到
  两个不同的 `RelationId`（自连接因此天然可分辨）。
- `ColumnId` —— 某个关系的一列，带关系内 `ordinal`。
- `ExpressionId` —— AST 中一个表达式节点，按前序分配。
- `ScopeId` —— 一层名字可见性。

### 作用域树

`ScopeTree` 持有 scope / relation / column 三张稠密表，父链表达嵌套：

- `resolveColumn(qualifier, name, from, &ambiguous)` 从 `from` 沿父链向外查找，
  返回 `ColumnRef{column, relation, correlationDepth}`。`correlationDepth > 0`
  就是相关子查询——相关性不再靠「外层列名是否出现在内层」这类猜测判定。
- 同一层出现多个候选时返回 `nullopt` 并置 `ambiguous`，歧义被报告而不是被猜。
- `resolveRelation` 先查本层 CTE 名，再查本层关系，最后向外层走。

**别名、派生表别名、CTE 名都是作用域名，不是数据库对象。** 这条不变量由数据
结构保证：CTE 经 `addCommonTableExpression` 登记进 `Scope::commonTableExpressions`，
而受权对象只在 `bindBaseRelation` 里产生，两条路径不相交。

### 受权对象带动作

`security::AccessRequest` 里每个对象带自己的 `AccessAction`，而不是整条语句
共用一个权限：

```
DELETE FROM a WHERE id IN (SELECT id FROM b)
  →  {a, Delete}, {b, Select}
```

这关掉了 access-control-progress.md 里记的「DELETE 中的子查询也可能被保守地
要求主表的 DELETE 权限」。

### WITH 成为真语法

`parser` 新增非递归 `WITH name [(columns)] AS ( SELECT ... )`（`Statement::ctes`）。
这是 fail-closed 能成立的前提：此前 CTE 完全不可解析，只能靠词法扫描兜底。
递归 CTE 显式报 `NotImplemented`，不会被静默当成普通标识符。

### 鉴权不再看 SQL 文本

- `security/access_catalog.cpp` 删除 `firstKeyword` / `sqlWords` /
  `tableReferences` / `sqlIdentifier` / `collectAst*` / `astTableReferences`
  共 187 行，并去掉对 `minisql/sql/lexer.hpp`、`minisql/sql/parser.hpp` 的
  依赖——`minisql_common` 不再反向依赖 SQL 层。
- `execution/database.cpp` 删除 `lexicalAccessObjects` 及其 `sqlIdentifier`，
  `resolveAccessObjects` 变成 `bindAccess` 的薄封装。
- `AccessCatalog::authorize` 的签名从 `(user, operation, sql, table, index,
  resolvedObjects)` 变为 `(user, operation, AccessRequest, table, index)`。
  **`request.bound == false` 一律拒绝执行**，不存在基于 SQL 文本的兜底扫描。
  拒绝与「回报什么错误」是两件事：`AccessRequest::failureCode/failureLocation`
  让拒绝抛出绑定失败的**真实**错误码（对象不存在 → `Catalog 3001`，解析期未实现
  构造 → `NotImplemented 9001`），只有拿不到失败原因时（例如序列化计划文档形状
  不合法）才回落到 `Permission 7001`。这样「表不存在」按第十九章映射成 422，
  而不是按授权失败报成 403；也消除了同一句 SQL 在是否配置权限目录两种情况下
  状态码不一致的问题。身份校验（`authorizeIdentity`）仍在对象授权之前完成，
  未授权调用方拿到的依旧是权限错误。

入口动作（`catalog` / `statistics` / `buffer` / `close` / `indexinspect` /
`snapshot` / `restore`）不携带 SQL，权限仍只由入口决定；`compile` /
`diagnostics` 把读动作降级为 `compile`，写动作不降级——与改造前一致。

## 验证

`tests/binding_identity_contract.cpp`（新增，`minisql_binding_identity_contract`）
覆盖：别名不是对象、自连接两个 `RelationId` 且同名列不歧义地各归其主、
派生表、CTE、具名列 CTE、CTE 与基表混用、按对象分派动作、字符串字面量防误报、
`ExpressionId` 稠密且携带列解析结果、`EXPLAIN` 前缀不改变绑定、
未知表/语法错误/递归 CTE 三种 fail-closed、事务语句无对象、序列化确定性。

既有 `tests/access_binding_contract.cpp` 的六条断言全部仍然成立，但结果来源
从「AST + 词法回退」变成了纯绑定：六条中此前依赖回退的四条（两条 CTE、
派生表、嵌套子查询）现在走结构化路径。

## 本轮实现（阶段二：planner 消费绑定 / 阶段三：优化器消费稳定身份）

### columnId 的两个身份被拆开

改造前 `PlanColumn::columnId` 同时兼任两件事：

- **运行时槽位**：执行器 `row.at(column.columnId)` 用它索引子节点产出的行；
  JOIN 把左右两侧的槽位拼接，所以谓词下推到右侧时必须整体减去左侧宽度。
- **列身份**：裁剪、下推、计划对比用它判断「这是哪一列」。

一个字段承担两种语义，任何改变槽位的规则都会顺带改变身份。现在拆成两个：

| 字段 | 语义 | 谁消费 |
| --- | --- | --- |
| `PlanColumn::columnId` | 运行时槽位，会随下推重基 | 执行器 |
| `PlanColumn::binding` | `sql::ColumnId`，跨改写不变 | 优化器、计划对比 |
| `PlanColumn::relation` | `sql::RelationId`，该列来自哪个 FROM 项 | 优化器归属判断 |

原先那个从未被任何代码读取的 `PlanColumn::identity`（`"qualifier.name"` 字符串）
被这两个字段取代。

### planner

`compilePlans` 逐句调用 `bindStatements({&statement}, snapshot)`——逐句而非整批，
因为批内 DDL 会改变 Catalog 快照。`build` 拿到 `BindResult` 后用
`slotIdentities` 把扁平作用域的每个槽位按 `(qualifier, name)` 在作用域树里查成
`ColumnId`/`RelationId`，再经 `slotWindow` 切出子节点（基表扫描、JOIN 右侧）
对应的窗口。身份从 SeqScan 一路贯穿到 Project。

绑定失败或语句不在绑定结果中时，`slots` 全为 0，计划照常按槽位语义构建——
这里不 fail-closed，因为 planner 有自己的语义诊断，鉴权线才是安全边界。

### 优化器

- `pruneProjectColumns` 改为按 `binding` 收集必需列。裁剪只是从 `output` 里移除
  若干列，**不重新编号**：留下的列槽位仍指向扫描行里的原位置，身份也不变。
- `pushPredicateIntoJoin` 的归属判断从 `columnId < leftSize` 改为比对
  `relation` 是否属于左/右子节点的关系集合。旧写法把「左子节点 output 的宽度」
  当成行宽的分界，一旦有规则裁剪过子节点 output，这个界就不再等于行宽，
  归属会静默算错——这是本轮关掉的一个潜在缺陷，而不只是重构。
- 推到右侧时仍然调用 `shiftColumns` 重基槽位，这是正确的：槽位本来就该重基。
  `shiftColumns` 只改 `columnId`，`binding`/`relation` 原样穿过，所以身份在重基
  前后可比——契约测试直接断言了这一点。
- `remove-true-filter` 的计划等价判断加入 `binding`/`relation`。

两条路径并存：没有身份的计划（旧文档、表达式列）退回原来的槽位判断。契约测试
里的差分检查对五条查询分别跑「带身份」与「抹掉身份」两条路径，要求输出除身份
字段外逐字节相同。

### 验证

`tests/optimizer_binding_contract.cpp`（新增，`minisql_optimizer_binding_contract`，
50 项检查，只依赖 `minisql_optimizer`，不需要 Database）：下推后身份不变而槽位
重基、自连接两侧关系身份可分辨且谓词归属正确、裁剪按身份进行且不重新编号、
序列化往返保留身份、以及上面的差分回归。

`minisql_planner_contract`、`minisql_parser_subquery_contract`、
`minisql_aggregate_semantic_contract`、`minisql_binding_identity_contract`
全部仍然通过（`optimizer_binding_contract` 现为 59 项，含相关子查询 GROUP BY
检查；`binding_identity_contract` 增加了外层引用的绑定器级断言）。绑定器另在 ASan/UBSan 下跑通——这一轮也借此修掉了阶段一留下的
两处悬垂指针（`bindFromName` 里 `addRelation` 之后继续读被重分配的
`BoundRelation*`；`bindSource` 解析出的语句在返回时析构而 `BoundExpression::node`
仍指向它们，现由 `BindResult::owned` 持有）。

## 后续阶段

### 阶段二 / 三遗留项：相关子查询的外层引用（已完成）

planner 里还剩一处 SQL 字符串扫描，和之前从 security 层删掉的是同一类问题：
`lowerAggregate` 的 `remapGroupRefs` 为了判断「这个相关子查询引用了哪些外层
列」，把 `subquerySql` **重新分词**，然后按 `IDENTIFIER . IDENTIFIER` 的模式
猜。这有两个后果：

- 未限定的外层引用（`WHERE b.id = x`，其中 `x` 只存在于外层）匹配不上任何
  `IDENT . IDENT` 模式，直接漏掉；
- 子查询里出现的、恰好形如 `x.y` 的内层引用会被拿去查外层作用域表。

绑定器本来就精确知道答案：子查询体内每个解析到「该子查询所在作用域或其祖先」
的列引用，就是一个外层引用。新增 `BindResult::correlatedReferences`
（`Expr*` -> `ColumnRef` 列表，按 `ColumnId` 去重）与
`ScopeTree::atOrAbove`，判定依据是作用域归属，不是列名文本，也不是相关深度的
算术。planner 把它写进计划节点的 `outerReferences`，`remapGroupRefs` 直接读。

这修掉了一个真实的语义缺陷，不只是重构。实测对比（`GROUP BY a.id`）：

| 查询 | 旧（重新分词） | 新（绑定器） |
| --- | --- | --- |
| `... WHERE b.id = a.x` | 报错「Column must be grouped or aggregated: x」 | 同样报错 |
| `... WHERE b.id = x` | **通过**（漏检） | 报错 |
| `... WHERE b.id = a.id` | 通过 | 通过 |

中间那行是漏检：`x` 没有被分组也没有被聚合，却放行了。

`correlatedScope` 产出的 `outerColumns` 现在每项额外带 `binding` / `relation`。
键暂时仍是 `"qualifier.name"`，因为执行器 (`database.cpp` 的 `bindOuter`) 目前
按这个键把内层 AST 的标识符文本绑到外层行槽位；执行器迁移到按身份绑定之后，
字符串键即可退役。

没有绑定信息时（旧计划文档）`outerReferences` 字段整个缺席，`remapGroupRefs`
退回原来的词法扫描，行为不变——这也是这里区分「字段为空数组」与「字段不存在」
的原因。

### 仍未闭合

- `catalog::queryScope` 仍在自己拼扁平作用域，planner 再按 `(qualifier, name)`
  查回绑定身份。要让 planner 直接遍历 `Scope::relations`，必须先把类型推导
  （`expressionType` / `analyzeSelect` / `resolveOrder`，它们都建立在
  `catalog::Table` 上）搬进绑定器。这是一次独立的架构步骤，不是收尾清理，
  不宜和本轮混在一起做。
- 执行器的 `OuterBinding` 仍按 `"qualifier.name"` 字符串匹配内层 AST 的标识符
  文本。`outerColumns` 已经带上了身份，迁移的材料齐了，但改动落在
  `src/execution/database.cpp`，需要完整构建环境验证。
- 规则记录（`changes` / `diagnostics`）仍靠 `expressionIdentity(...).dump()` 的
  字符串比对对齐改写前后的节点。`ExpressionId` 已经分配好，但还没写进计划
  文档——与阶段四绑在一起做。

### 阶段四：序列化统一（未完成）

计划文档现在写 `binding` / `relation`（可选字段，缺失回落 0），`identity` 字段
移除。两个方向都能优雅降级，因此**没有**做 major 版本变更：新读端读旧文档得到
0（无身份，退回槽位语义），旧读端读新文档忽略未知字段。

待办：把 `expressionId` 一并写进计划节点，并把 `serializeBinding` 的输出作为
计划文档的 `binding` 段落。届时按 X13 规则应当提升次版本。

> 注意：`docs/ast-plan-schema-versioning.md` 描述了 `PLAN_SCHEMA_MINOR`、
> `PLAN_VERSION`、`AST_SCHEMA_MINOR` 与 `versionReadable` 闸门，但
> `include/minisql/sql/serialization.hpp` 里目前只有 `AST_SCHEMA_VERSION`、
> `PLAN_SCHEMA_VERSION`、`PRODUCER_VERSION`、`CATALOG_SCHEMA_VERSION`。
> 文档与代码不一致，阶段四开始前需要先对齐，否则「提升次版本」无处可提。

### 阶段五：清理过渡字段

`Expr::subquerySql` 在绑定器里已被一次性结构化（解析失败即 fail-closed）。
阶段二完成后 planner 不再读它，即可随 X09 的收尾一起删除，`Expr::subquery`
成为唯一表示。

## 限制

- 递归 CTE 未实现，显式报 `NotImplemented`。
- 派生表 / CTE 的输出列名在含通配符或复杂表达式时留空，这些列因此没有稳定身份
  （`binding == 0`），优化器对它们退回槽位语义。对象绑定是否闭合不受影响。
- 聚合输出、`_sort_` 临时列、表达式列同样没有身份，这是设计如此：它们不是任何
  关系的列。
- 列解析失败（未知列、歧义）不会清除 `BindResult::complete`，因为对象集合仍然
  闭合、鉴权仍然安全；这类错误由语义层在计划阶段给出更精确的诊断。
- 阶段二、三的改动没有触碰执行器：`columnId` 的槽位语义完全不变，因此
  `src/execution/database.cpp` 的 `row.at(column.columnId)` 无需同步修改。
- HTTP bridge（`scripts/database-bridge.mjs`）与 CLI 侧的 JS 词法扫描未在本轮
  改动。它们只做「早拒」，真正的授权判定已经完全在 C++ 绑定结果上；JS 侧的
  扫描应在阶段二之后随 `bindAccess` 的 HTTP 暴露一并删除。
