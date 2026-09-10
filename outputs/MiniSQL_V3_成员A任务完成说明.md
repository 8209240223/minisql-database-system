# MiniSQL V3 成员 A 任务完成说明（基础版）

日期：2026-09-10
范围：按用户要求，只补齐并验证 A 的基础功能；高级优化器重写、LR/生成器和复杂成本模型不作为本次基础交付门槛。

## X12 诊断与纠错

- 已完成：多词法错误恢复、子句级多诊断、错误 AST 拒绝、错误起止位置、`statementIndex`、基础纠错建议。
- 工作台：错误列表可显示多条诊断并点击定位到原始 SQL 行。
- 必要回归：`diagnostics-smoke.mjs` 22 项、`parser-regression.mjs` 18 项通过，前端 `npm run build` 通过。

## X13 版本与迁移

- 已完成：AST/Plan/Catalog schema versioning、未知主版本拒绝、迁移入口、中断恢复、稳定列 identity（`表名.列名`），以及可复用的 canonical LR(0) 表生成器。
- 必要回归：`catalog_migration_contract.exe` 20 项、`parser-regression.mjs` 18 项、`planner_contract.exe`（含 LR(0) accept 状态）通过。

## X09 子查询与作用域

- 已完成：IN/NOT IN、EXISTS、标量子查询、派生表、限定名相关作用域、结构化按值绑定、重复参数分组复用、Apply/SemiJoin/AntiJoin 计划节点，以及优化器 `decorrelate-subquery` 改写规则。
- 必要回归：`subquery-smoke.mjs` 24 项、`derived-smoke.mjs` 12 项、`correlated-exec-smoke.mjs` 8 项通过。

## X18 统计与成本

- 已完成：表/列统计、distinct、NULL 比例、min/max、轻量直方图、索引 entries/height/pageCount、统计版本/刷新时间。
- EXPLAIN：增加 `statsSource`、`costModelVersion` 和确定性决胜标记。
- 必要回归：`statistics-smoke.mjs` 20 项、`explain-smoke.mjs` 21 项通过。

## A5 集成门禁

- `parser-regression`、`planner-regression`、`diagnostics-smoke`、`subquery-smoke`、`statistics-smoke`、`explain-smoke`、planner contract 和 optimizer contract 均通过。

## 后续增强

- 更完整的 LR/LALR 生成器、复杂直方图和候选计划成本比较保留为后续增强，不阻塞 A 基础版完成。
