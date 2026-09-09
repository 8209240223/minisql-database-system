# MiniSQL 数据库测试脚本

这些脚本可以直接通过工作台的“导入 SQL”按钮加载并执行。读查询只读取当前真实 C++ 数据库；写操作全部包在 `BEGIN ... ROLLBACK` 中，不会留下测试数据。

## 脚本清单

| 文件 | 测试内容 |
| --- | --- |
| `01_basic_select.sql` | 基础 SELECT、排序和 LIMIT |
| `02_join_query.sql` | 三表 JOIN |
| `03_group_aggregate.sql` | GROUP BY、COUNT、AVG |
| `04_sort_filter_limit.sql` | WHERE、排序、分页 |
| `05_in_subquery.sql` | IN 子查询 |
| `06_exists_subquery.sql` | EXISTS 相关子查询 |
| `07_null_three_value.sql` | NULL 三值逻辑 |
| `08_update_rollback.sql` | UPDATE 和事务回滚 |
| `09_delete_rollback.sql` | DELETE 和事务回滚 |
| `10_constraints.sql` | 主键、NOT NULL、CHECK、UNIQUE、多行 INSERT |
| `11_index_scan.sql` | CREATE INDEX 和索引查询 |
| `12_multirow_insert.sql` | 多行 INSERT |
| `13_count_all.sql` | 三张表行数统计 |

## 推荐顺序

1. 先运行 `13_count_all.sql` 确认数据库有数据。
2. 运行 `01_basic_select.sql` 和 `02_join_query.sql` 检查基础查询。
3. 运行 `03_group_aggregate.sql` 和 `04_sort_filter_limit.sql` 检查聚合与排序。
4. 运行 `05_in_subquery.sql` 和 `06_exists_subquery.sql` 检查子查询。
5. 运行 `07_null_three_value.sql` 到 `12_multirow_insert.sql` 检查事务、约束和索引。
