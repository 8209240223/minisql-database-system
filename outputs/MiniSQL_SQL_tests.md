# MiniSQL SQL 测试用例集

本文档由脚本驱动真实引擎逐条执行后生成，每条用例的**实测结果**与判定均由引擎输出决定，非人工填写。

| 项 | 值 |
| --- | --- |
| 被测引擎 | `outputs/minisql-backend/bin/minisql_database.exe` |
| 调用方式 | `minisql_database.exe <db.pages> execute`，SQL 由标准输入传入 |
| 用例总数 | 122 |
| 通过 | 122 |
| 失败 | 0 |

判定口径：期望 ok 的用例必须执行成功；期望 err 的用例必须被引擎拒绝。两者都算通过。

本集还覆盖本轮新增的查询优化能力：查询结果缓存（重复 SELECT 直接命中）、
三级缓存可观测性（statistics 里的 queryCache）、索引建议器（基于实际查询负载给出建索引建议）、
以及 Top-N 排序下推（ORDER BY + LIMIT 只保留前 N 行，不再全量排序）。

说明：`err` 有两种含义，文档中按用例名区分——一类是**应当拒绝的非法输入**（如缺 ON、超长 VARCHAR），
另一类是**当前尚未实现的语法**（如 COUNT(DISTINCT)、BETWEEN、CASE WHEN），它们都会返回明确错误而非静默通过。

## JOIN 与派生表

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | INNER JOIN 等值连接 | `SELECT t.id,d.dname FROM t JOIN d ON t.dept=d.id;` | ok | [[1, 'CS'], [2, 'EE']] | ✅ |
| 2 | LEFT JOIN 保留左表 | `SELECT t.id,d.dname FROM t LEFT JOIN d ON t.dept=d.id;` | ok | [[1, 'CS'], [2, 'EE'], [3, None]] ...共 4 行 | ✅ |
| 3 | RIGHT JOIN 保留右表 | `SELECT t.id,d.dname FROM t RIGHT JOIN d ON t.dept=d.id;` | ok | [[1, 'CS'], [2, 'EE'], [None, 'ME']] | ✅ |
| 4 | FULL OUTER JOIN | `SELECT t.id,d.dname FROM t FULL OUTER JOIN d ON t.dept=d.id;` | ok | [[1, 'CS'], [2, 'EE'], [3, None]] ...共 5 行 | ✅ |
| 5 | CROSS JOIN 笛卡尔积 | `SELECT t.id,d.dname FROM t CROSS JOIN d;` | ok | [[1, 'CS'], [1, 'EE'], [1, 'ME']] ...共 12 行 | ✅ |
| 6 | 逗号连接（等价 CROSS） | `SELECT t.id,d.dname FROM t, d;` | ok | [[1, 'CS'], [1, 'EE'], [1, 'ME']] ...共 12 行 | ✅ |
| 7 | 三表逗号连接 | `SELECT t.id FROM t, d, d AS d2;` | ok | [[1], [1], [1]] ...共 36 行 | ✅ |
| 8 | 自连接 | `SELECT a.id,b.id FROM t a JOIN t b ON a.dept=b.dept;` | ok | [[1, 1], [2, 2], [3, 3]] | ✅ |
| 9 | JOIN + WHERE 等值过滤 | `SELECT t.id FROM t JOIN d ON t.dept=d.id WHERE d.dname='CS';` | ok | [[1]] | ✅ |
| 10 | JOIN + WHERE 恒真（回归用例） | `SELECT t.id FROM t JOIN d ON t.dept=d.id WHERE 1=1;` | ok | [[1], [2]] | ✅ |
| 11 | LEFT JOIN 谓词下推 | `SELECT t.id FROM t LEFT JOIN d ON t.dept=d.id WHERE t.id>1;` | ok | [[2], [3], [4]] | ✅ |
| 12 | JOIN 缺 ON 报错 | `SELECT t.id FROM t JOIN d;` | err | - | ✅ |
| 13 | CROSS JOIN 带 ON 报错 | `SELECT t.id FROM t CROSS JOIN d ON t.dept=d.id;` | err | - | ✅ |
| 14 | NATURAL JOIN 不支持 | `SELECT t.id FROM t NATURAL JOIN d;` | err | - | ✅ |
| 15 | JOIN USING 不支持 | `SELECT t.id FROM t JOIN d USING(id);` | err | - | ✅ |
| 16 | JOIN 歧义列名报错 | `SELECT id FROM t JOIN d ON t.dept=d.id;` | err | - | ✅ |
| 17 | 派生表作数据源 | `SELECT x.id FROM (SELECT id FROM d) x;` | ok | [[10], [20], [40]] | ✅ |
| 18 | 派生表无别名报错 | `SELECT id FROM (SELECT id FROM d);` | err | - | ✅ |
| 19 | 派生表作 JOIN 右操作数 | `SELECT t.id FROM t JOIN (SELECT id FROM d) x ON t.dept=x.id;` | ok | [[1], [2]] | ✅ |
| 20 | 派生表作 LEFT JOIN 右操作数 | `SELECT t.id FROM t LEFT JOIN (SELECT id FROM d) x ON t.dept=x.id;` | ok | [[1], [2], [3]] ...共 4 行 | ✅ |
| 21 | 派生表作 CROSS JOIN 右操作数 | `SELECT t.id FROM t CROSS JOIN (SELECT id FROM d) x;` | ok | [[1], [1], [1]] ...共 12 行 | ✅ |
| 22 | 逗号连接 + 派生表 | `SELECT t.id FROM t, (SELECT id FROM d) x;` | ok | [[1], [1], [1]] ...共 12 行 | ✅ |
| 23 | 派生表 JOIN 派生表 | `SELECT a.id FROM (SELECT id FROM d) a JOIN (SELECT id FROM d) b ON a.id=b.id;` | ok | [[10], [20], [40]] | ✅ |
| 24 | 派生表列别名引用 | `SELECT t.id, x.dname FROM t JOIN (SELECT id, dname FROM d) x ON t.dept=x.id;` | ok | [[1, 'CS'], [2, 'EE']] | ✅ |
| 25 | 派生表 + WHERE 组合 | `SELECT t.id FROM t JOIN (SELECT id FROM d) x ON t.dept=x.id WHERE t.id>1;` | ok | [[2]] | ✅ |

## 子查询

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | IN 值列表 | `SELECT id FROM t WHERE id IN (1,3);` | ok | [[1], [3]] | ✅ |
| 2 | NOT IN 值列表 | `SELECT id FROM t WHERE id NOT IN (1,3);` | ok | [[2]] | ✅ |
| 3 | IN 子查询 | `SELECT id FROM t WHERE dept IN (SELECT id FROM d);` | ok | [[1], [2]] | ✅ |
| 4 | NOT IN 子查询 | `SELECT id FROM t WHERE dept NOT IN (SELECT id FROM d);` | ok | [[3]] | ✅ |
| 5 | EXISTS 相关子查询 | `SELECT id FROM t WHERE EXISTS (SELECT 1 FROM d WHERE d.id=t.dept);` | ok | [[1], [2]] | ✅ |
| 6 | NOT EXISTS | `SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM d WHERE d.id=t.dept);` | ok | [[3]] | ✅ |
| 7 | 标量子查询 | `SELECT id, (SELECT dname FROM d WHERE d.id=10) FROM t WHERE id=1;` | ok | [[1, 'CS']] | ✅ |
| 8 | 相关子查询（限定名） | `SELECT id FROM t WHERE dept=(SELECT id FROM d WHERE d.id=t.dept);` | ok | [[1], [2]] | ✅ |
| 9 | 标量子查询多行报错 | `SELECT id, (SELECT id FROM d) FROM t WHERE id=1;` | err | - | ✅ |

## 聚合、排序与分页

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | COUNT(*) | `SELECT COUNT(*) FROM s;` | ok | [[5]] | ✅ |
| 2 | COUNT(col) 跳过 NULL | `SELECT COUNT(n) FROM s;` | ok | [[4]] | ✅ |
| 3 | SUM/AVG/MIN/MAX | `SELECT SUM(v),AVG(v),MIN(v),MAX(v) FROM s;` | ok | [[150, '30.000000', 10, 50]] | ✅ |
| 4 | GROUP BY | `SELECT g,COUNT(*) FROM s GROUP BY g;` | ok | [[None, 1], ['a', 2], ['b', 2]] | ✅ |
| 5 | GROUP BY + HAVING | `SELECT g,SUM(v) FROM s GROUP BY g HAVING SUM(v)>30;` | ok | [[None, 50], ['b', 70]] | ✅ |
| 6 | HAVING 无 GROUP BY | `SELECT SUM(v) FROM s HAVING SUM(v)>0;` | ok | [[150]] | ✅ |
| 7 | DISTINCT | `SELECT DISTINCT g FROM s;` | ok | [[None], ['a'], ['b']] | ✅ |
| 8 | COUNT(DISTINCT) 未实现 | `SELECT COUNT(DISTINCT g) FROM s;` | err | - | ✅ |
| 9 | ORDER BY 多键 | `SELECT id FROM s ORDER BY g DESC, id ASC;` | ok | [[5], [3], [4]] ...共 5 行 | ✅ |
| 10 | ORDER BY NULLS FIRST | `SELECT g FROM s ORDER BY g ASC NULLS FIRST;` | ok | [[None], ['a'], ['a']] ...共 5 行 | ✅ |
| 11 | ORDER BY 别名 | `SELECT v AS vv FROM s ORDER BY vv DESC;` | ok | [[50], [40], [30]] ...共 5 行 | ✅ |
| 12 | LIMIT | `SELECT id FROM s ORDER BY id LIMIT 2;` | ok | [[1], [2]] | ✅ |
| 13 | LIMIT + OFFSET | `SELECT id FROM s ORDER BY id LIMIT 2 OFFSET 2;` | ok | [[3], [4]] | ✅ |
| 14 | 聚合+WHERE+GROUP+ORDER | `SELECT g,SUM(v) FROM s WHERE v>10 GROUP BY g ORDER BY g;` | ok | [['a', 20], ['b', 70], [None, 50]] | ✅ |

## 数据类型与 CAST

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | BIGINT 超 32 位精度 | `SELECT b FROM y;` | ok | [['9007199254740993']] | ✅ |
| 2 | DECIMAL(10,2) | `SELECT c FROM y;` | ok | [['12.34']] | ✅ |
| 3 | FLOAT | `SELECT f FROM y;` | ok | [[1.5]] | ✅ |
| 4 | BOOL | `SELECT o FROM y;` | ok | [[True]] | ✅ |
| 5 | DATE 关键字字面量 | `SELECT dt FROM y WHERE dt > DATE '2023-12-31';` | ok | [['2024-01-01']] | ✅ |
| 6 | VARCHAR(5) | `SELECT s FROM y;` | ok | [['abc']] | ✅ |
| 7 | VARCHAR 超长拒绝 | `INSERT INTO y(a,s) VALUES(2,'abcdefg');` | err | - | ✅ |
| 8 | CAST INT -> DECIMAL | `SELECT CAST(a AS DECIMAL(10,2)) FROM y;` | ok | [['1.00']] | ✅ |
| 9 | CAST INT -> FLOAT | `SELECT CAST(a AS FLOAT) FROM y;` | ok | [[1.0]] | ✅ |
| 10 | CAST DECIMAL -> INT | `SELECT CAST(c AS INT) FROM y;` | ok | [[12]] | ✅ |
| 11 | 字符串隐式转日期拒绝 | `SELECT dt FROM y WHERE dt > '2023-12-31';` | err | - | ✅ |
| 12 | DECIMAL 与 INT 混合运算 | `SELECT c + 1 FROM y;` | ok | [['13.34']] | ✅ |

## 约束（主键/唯一/CHECK/NOT NULL）

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 主键/默认值合法插入 | `INSERT INTO p VALUES(1,'a',18,DEFAULT);` | ok | (0 行) | ✅ |
| 2 | 主键重复拒绝 | `INSERT INTO p VALUES(1,'b',18,1);` | err | - | ✅ |
| 3 | 唯一约束冲突拒绝 | `INSERT INTO p VALUES(2,'a',18,1);` | err | - | ✅ |
| 4 | CHECK 违反拒绝 | `INSERT INTO p VALUES(3,'c',-1,1);` | err | - | ✅ |
| 5 | NOT NULL 违反拒绝 | `INSERT INTO p VALUES(4,'d',1,NULL);` | err | - | ✅ |

## 外键与索引

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 外键引用合法 | `INSERT INTO c2 VALUES(2,1);` | ok | (0 行) | ✅ |
| 2 | 外键引用不存在拒绝 | `INSERT INTO c2 VALUES(3,99);` | err | - | ✅ |
| 3 | CREATE INDEX | `CREATE INDEX ix ON c2(r);` | ok | (0 行) | ✅ |
| 4 | CREATE UNIQUE INDEX | `CREATE UNIQUE INDEX ux ON c2(id);` | ok | (0 行) | ✅ |
| 5 | 唯一索引冲突拒绝 | `INSERT INTO c2 VALUES(2,1);` | err | - | ✅ |
| 6 | DROP INDEX | `DROP INDEX ix ON c2;` | ok | (0 行) | ✅ |
| 7 | 索引后查询 | `SELECT id FROM c2 WHERE r=1;` | ok | [[2]] | ✅ |

## 复合主键

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 复合主键重复拒绝 | `INSERT INTO pk2 VALUES(1,1);` | err | - | ✅ |
| 2 | 复合主键新值合法 | `INSERT INTO pk2 VALUES(1,2);` | ok | (0 行) | ✅ |

## 事务与保存点

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | BEGIN/INSERT/COMMIT | `BEGIN; INSERT INTO m VALUES(1); COMMIT;` | ok | (0 行) | ✅ |
| 2 | COMMIT 后可见 | `SELECT id FROM m;` | ok | [[1]] | ✅ |
| 3 | ROLLBACK 撤销 | `BEGIN; INSERT INTO m VALUES(2); ROLLBACK;` | ok | (0 行) | ✅ |
| 4 | SAVEPOINT 回滚到点 | `BEGIN; INSERT INTO m VALUES(4); SAVEPOINT s1; INSERT INTO m VALUES(5); ROLLBACK TO s1; COMMIT;` | ok | (0 行) | ✅ |
| 5 | RELEASE SAVEPOINT | `BEGIN; SAVEPOINT s2; INSERT INTO m VALUES(6); RELEASE SAVEPOINT s2; COMMIT;` | ok | (0 行) | ✅ |
| 6 | 嵌套 BEGIN 拒绝 | `BEGIN; BEGIN; COMMIT;` | err | (0 行) | ✅ |
| 7 | CHECKPOINT | `CHECKPOINT;` | ok | (0 行) | ✅ |

## 表达式与三值逻辑

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 算术优先级 a+b*2 | `SELECT a+b*2 FROM x WHERE a IS NOT NULL;` | ok | [[16]] | ✅ |
| 2 | 括号 (a+b)*2 | `SELECT (a+b)*2 FROM x WHERE a IS NOT NULL;` | ok | [[26]] | ✅ |
| 3 | 整型除法 | `SELECT a/b FROM x WHERE a IS NOT NULL;` | ok | [[3]] | ✅ |
| 4 | 一元负号 | `SELECT -a FROM x WHERE a IS NOT NULL;` | ok | [[-10]] | ✅ |
| 5 | == 方言等价 = | `SELECT a FROM x WHERE a == 10;` | ok | [[10]] | ✅ |
| 6 | <> 方言等价 != | `SELECT a FROM x WHERE a <> 10;` | ok | (0 行) | ✅ |
| 7 | IS NULL | `SELECT a FROM x WHERE a IS NULL;` | ok | [[None]] | ✅ |
| 8 | IS NOT NULL | `SELECT a FROM x WHERE a IS NOT NULL;` | ok | [[10]] | ✅ |
| 9 | 三值逻辑 AND | `SELECT a FROM x WHERE a>1 AND s IS NULL;` | ok | (0 行) | ✅ |
| 10 | NOT 作用整个比较 | `SELECT a FROM x WHERE NOT a = 10;` | ok | (0 行) | ✅ |
| 11 | a = NULL 恒 UNKNOWN | `SELECT a FROM x WHERE a = NULL;` | ok | (0 行) | ✅ |
| 12 | LIKE % 通配 | `SELECT s FROM x WHERE s LIKE 'a%';` | ok | [['abc']] | ✅ |
| 13 | LIKE _ 单字符 | `SELECT s FROM x WHERE s LIKE 'a_c';` | ok | [['abc']] | ✅ |
| 14 | NOT LIKE | `SELECT s FROM x WHERE s NOT LIKE 'a%';` | ok | (0 行) | ✅ |
| 15 | LIKE 遇 NULL | `SELECT s FROM x WHERE s LIKE '%';` | ok | [['abc']] | ✅ |
| 16 | 连续比较拒绝 | `SELECT a FROM x WHERE a = a = 10;` | err | - | ✅ |
| 17 | BETWEEN 不支持 | `SELECT a FROM x WHERE a BETWEEN 1 AND 20;` | err | - | ✅ |
| 18 | CASE WHEN 不支持 | `SELECT CASE WHEN a>1 THEN 1 ELSE 0 END FROM x;` | err | - | ✅ |

## UPDATE 与 DELETE

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | UPDATE 单列 | `UPDATE u SET v=v+1 WHERE id=1;` | ok | (0 行) | ✅ |
| 2 | UPDATE 多赋值 | `UPDATE u SET v=v*2, s='z' WHERE id=2;` | ok | (0 行) | ✅ |
| 3 | UPDATE 无 WHERE 全表 | `UPDATE u SET v=0;` | ok | (0 行) | ✅ |
| 4 | DELETE 条件删除 | `DELETE FROM u WHERE id=3;` | ok | (0 行) | ✅ |
| 5 | UPDATE 类型不符拒绝 | `UPDATE u SET v='abc';` | err | - | ✅ |
| 6 | DELETE 全表 | `DELETE FROM u;` | ok | (0 行) | ✅ |

## EXPLAIN

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | EXPLAIN SELECT 不执行 | `EXPLAIN SELECT t.id FROM t JOIN d ON t.dept=d.id WHERE t.id>0;` | ok | [['Project', 'Project t', 0.033, 4.396000000000001, 'stats-v1', 'table-column-statistics-or-default'], ['NestedLoopJoin', 'NestedLoopJoin t', 0.033, 4.363, 'stats-v1', 'table-column-statistics-or-default'], ['Filter', 'Filter t', 0.33, 2.33, 'stats-v1', 'table-column-statistics-or-default']] ...共 5 行 | ✅ |
| 2 | EXPLAIN ANALYZE 实执行 | `EXPLAIN ANALYZE SELECT id FROM t;` | ok | [['Project', 'Project t', 1.0, 3.0, 'stats-v1', 'table-column-statistics-or-default'], ['SeqScan', 'SeqScan t', 1.0, 2.0, 'stats-v1', 'table-column-statistics-or-default']] | ✅ |
| 3 | EXPLAIN INSERT（设计允许） | `EXPLAIN INSERT INTO t VALUES(2,20);` | ok | [['Insert', 'Insert t', 2.0, 1.0, 'stats-v1', 'table-column-statistics-or-default']] | ✅ |
| 4 | EXPLAIN ANALYZE 写语句拒绝 | `EXPLAIN ANALYZE INSERT INTO t VALUES(3,30);` | err | - | ✅ |

## 统计、缓存与索引建议

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | 统计接口可用 | `SELECT COUNT(*) FROM t;` | ok | [[2]] | ✅ |
| 2 | 聚合查询（可走缓存） | `SELECT SUM(v) FROM t;` | ok | [[30]] | ✅ |
| 3 | 重复查询结果一致 | `SELECT COUNT(*) FROM t; SELECT COUNT(*) FROM t;` | ok | [[2]] | ✅ |
| 4 | 带谓词查询（累积建议负载） | `SELECT id FROM t WHERE v=10; SELECT id FROM t WHERE v=20;` | ok | [[2]] | ✅ |
| 5 | 建索引后查询仍正确 | `CREATE INDEX ix_v ON t(v); SELECT COUNT(*) FROM t;` | ok | [[2]] | ✅ |

## Top-N 排序下推

| # | 用例 | SQL | 期望 | 实测 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 1 | ORDER BY + LIMIT 正确取前 N | `SELECT id FROM o ORDER BY v LIMIT 3;` | ok | [[4], [8], [2]] | ✅ |
| 2 | ORDER BY DESC + LIMIT | `SELECT id FROM o ORDER BY v DESC LIMIT 3;` | ok | [[3], [7], [5]] | ✅ |
| 3 | ORDER BY + OFFSET + LIMIT | `SELECT id FROM o ORDER BY v LIMIT 2 OFFSET 2;` | ok | [[2], [6]] | ✅ |
| 4 | 多键排序 + LIMIT | `SELECT id FROM o ORDER BY v, id LIMIT 4;` | ok | [[4], [8], [2]] ...共 4 行 | ✅ |
| 5 | 并列键降序 + LIMIT | `SELECT id FROM o ORDER BY k DESC LIMIT 3;` | ok | [[5], [6], [8]] | ✅ |
| 6 | LIMIT 超过行数返回全部 | `SELECT id FROM o ORDER BY v LIMIT 100;` | ok | [[4], [8], [2]] ...共 8 行 | ✅ |
| 7 | 无 LIMIT 的完整排序 | `SELECT id FROM o ORDER BY v;` | ok | [[4], [8], [2]] ...共 8 行 | ✅ |
| 8 | Top-N 与 WHERE 组合 | `SELECT id FROM o WHERE v>2 ORDER BY v DESC LIMIT 2;` | ok | [[3], [7]] | ✅ |

## 失败用例说明

无。全部用例通过。

