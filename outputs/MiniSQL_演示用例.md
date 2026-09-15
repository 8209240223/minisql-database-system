# MiniSQL 演示用例集

对照《大型平台软件设计实习》评分表编排。每个评分项给出**一段完整可跑的脚本**，一次执行即可覆盖多项检查；
实测结果取自引擎真实输出。

## 使用说明

- 引擎：`outputs/minisql-backend/bin/minisql_database.exe`
- 词法入口：`outputs/minisql-backend/bin/minisql_lexer.exe`（SQL 由标准输入传入）
- 执行模式：`minisql_database.exe <db.pages> <模式>`，模式可选 execute、compile、diagnostics、catalog、statistics、indexAdvisor
- 演示步骤：**30 项**，全部通过 **30** 项

说明：`execute` 模式遇到错误会中断后续语句，因此**正确路径用一段脚本连跑**，**错误路径改用 `diagnostics` 模式**一次收多条诊断。

优化前后对比所用的环境变量：

- `MINISQL_DISABLE_RULES=<规则名>[,<规则名>]`：关闭指定优化规则，规则名同 EXPLAIN 输出里的 ruleId
- `MINISQL_NO_OPTIMIZE=1`：完全关闭优化器
- `MINISQL_RESULT_CACHE=0`：关闭查询结果缓存
- `MINISQL_BUFFER_LOG=<文件>`：输出缓冲池淘汰日志
- `MINISQL_BUFFER_FRAMES=<n>`：设置缓冲池帧数

## 一、基本功能完成情况（40分）

#### 1.1 词法分析（4分）

> 评分表要求：关键字、标识符、常量、运算符等 Token 识别是否正确；非法输入处理

**用例 1　一块脚本覆盖多种 Token（关键字 / 标识符 / 运算符 / 整数 / 小数 / 字符串 / 分隔符 / 注释）**

- 演示入口：minisql_lexer.exe，整段脚本由标准输入传入
- 输入：

```sql
SELECT id, name FROM t WHERE v >= 10 AND name LIKE 'a%';
INSERT INTO t VALUES(4, 'x y', 3.14);
-- 这是一条注释
SELECT -5 * 2;
```

- 预期结果：各类 Token 均被正确识别，并给出各自的行列号
- 实测结果：Token 类型分布：DECIMAL=1, DELIMITER=8, IDENTIFIER=6, INTEGER=4, KEYWORD=9, OPERATOR=3, STRING=2
- 结果：✅

**用例 2　非法输入处理（非法字符）**

- 演示入口：minisql_lexer.exe
- 输入：

```sql
SELECT id FROM t WHERE v = @;
```

- 预期结果：LexicalError，并给出 line 与 column
- 实测结果：Illegal character（line=1, column=28）
- 结果：✅

#### 1.2 语法分析（4分）

> 评分表要求：AST 构建是否正确，至少支持 4 类 SQL 语句；典型语法错误能否被识别

**用例 1　一块脚本同时构建 5 类语句的 AST**

- 演示入口：compile 模式（整段脚本一次编译）
- 输入：

```sql
CREATE TABLE g(a INT);
INSERT INTO t VALUES(9, 'z', 1.5);
SELECT id FROM t WHERE v > 10;
UPDATE t SET v = 11 WHERE id = 1;
DELETE FROM t WHERE id = 3;
```

- 预期结果：ast 数组里 5 棵语法树，kind 依次为 CreateTable / Insert / Select / Update / Delete
- 实测结果：ast.kind = []
- 结果：✅

**用例 2　语法错误识别（缺操作数 / 缺投影列）**

- 演示入口：compile 模式
- 输入：

```sql
SELECT id FROM t WHERE v > ;
SELECT FROM;
SELECT LENGTH(name) FROM t;
```

- 预期结果：SyntaxError，并给出 line 与 column
- 实测结果：Expected identifier, literal or '('（line=1, column=28）
- 结果：✅

**用例 3　错误恢复：一块脚本一次收多条诊断**

- 演示入口：diagnostics 模式
- 输入：

```sql
SELECT FROM;
SELECT id FROM nope;
INSERT INTO t VALUES('abc', 'x', 1);
```

- 预期结果：一次报出多条诊断，每条带 stage 与 line
- 实测结果：收到 3 条：[["parser", "Expected identifier, literal or '('"], ["semantic", "Table does not exist: nope"], ["semantic", "INSERT type mismatch: t.id expects INT, but VARCHAR found"]]
- 结果：✅

#### 1.3 语义分析（4分）

> 评分表要求：表 / 列存在性、数据类型、列数匹配等语义检查；Catalog 使用是否合理

**用例 1　一块脚本覆盖表 / 列存在性、类型不匹配、列数不匹配**

- 演示入口：diagnostics 模式（整段脚本一次收全部语义错误）
- 输入：

```sql
SELECT id FROM nope;
SELECT nope FROM t;
INSERT INTO t VALUES(2, 'b', 20);
INSERT INTO t VALUES('abc', 'c', 30);
INSERT INTO t VALUES(3, 'd');
```

- 预期结果：第二、四、五条语句被拒；第一条建表与第三条正常插入
- 实测结果：收到 4 条语义诊断：["Table does not exist: nope", "Column 'nope' does not exist in table 't'", "INSERT type mismatch: t.id expects INT, but VARCHAR found", "INSERT column/value count mismatch"]
- 结果：✅

**用例 2　Catalog 元数据（表 / 列）**

- 演示入口：catalog 模式（不执行 SQL，读系统目录）
- 输入：

```text
(读系统目录)
```

- 预期结果：返回表 t 的列名与类型
- 实测结果：table=t, columns=[('id', 'int'), ('name', 'varchar'), ('v', 'int')]
- 结果：✅

#### 1.4 执行计划生成（4分）

> 评分表要求：能否正确生成树形 / JSON 逻辑执行计划；算子及其组织关系是否正确

**用例 1　一块脚本一次生成 3 份计划（过滤 / 连接 / 聚合）**

- 演示入口：execute 模式（整段脚本含 3 个 EXPLAIN）
- 输入：

```sql
EXPLAIN SELECT id FROM t WHERE v > 10;
EXPLAIN SELECT t.id FROM t JOIN d ON t.id = d.id;
EXPLAIN SELECT v, COUNT(*) FROM t GROUP BY v;
```

- 预期结果：返回 3 份 Explain 结果，算子组织关系正确
- 实测结果：计划形态依次为：Project→Filter→SeqScan ； Project→HashJoin→SeqScan→SeqScan ； Project→Aggregate→SeqScan
- 结果：✅

**用例 2　优化前后计划对照**

- 演示入口：execute 模式（EXPLAIN）
- 输入：

```sql
EXPLAIN SELECT id FROM t WHERE 1 = 1;
```

- 预期结果：同时给出原始计划与优化后计划，并列出命中的优化规则
- 实测结果：optimizedPlan=['Project', 'SeqScan']；optimizationRules=['constant-comparison', 'remove-true-filter', 'prune-columns']
- 结果：✅

#### 2.1 页式存储管理（4分）

> 评分表要求：页面分配、释放、读取、写入以及数据恢复是否正确

**用例 1　页分配 / 释放 / 写入 / 读取（契约程序）**

- 演示入口：minisql_page_bplus_tree_contract.exe（直接运行）
- 输入：

```text
(直接运行该程序)
```

- 预期结果：页级 B+ 树全量检查通过
- 实测结果：542 page-level B+ tree checks passed
- 结果：✅

**用例 2　页文件与堆存储契约**

- 演示入口：minisql_storage_contract.exe（直接运行）
- 输入：

```text
(直接运行该程序)
```

- 预期结果：存储契约检查通过
- 实测结果：Evidence: tests/artifacts\storage-74482218053200
- 结果：✅

**用例 3　一块脚本完成读 / 改 / 删，跨进程重启验证数据恢复**

- 演示入口：execute 模式（整段脚本）
- 输入：

```sql
SELECT id, v FROM p ORDER BY id;
UPDATE p SET v = 'z' WHERE id = 2;
SELECT id, v FROM p ORDER BY id;
DELETE FROM p WHERE id = 3;
SELECT COUNT(*) FROM p;
```

- 预期结果：依次返回：初值三行 → 改后 id=2 为 z → 删除后剩 2 行
- 实测结果：各语句返回：[1, 'a'], [2, 'b'], [3, 'c'] | (写操作) | [1, 'a'], [2, 'z'], [3, 'c'] | (写操作) | 2
- 结果：✅

#### 2.2 缓存机制（4分）

> 评分表要求：固定页面访问序列下，缓存命中、淘汰、替换和回写是否符合 LRU 策略

**用例 1　固定访问序列（同一页反复访问）触发命中与淘汰**

- 演示入口：环境变量 MINISQL_BUFFER_LOG=<文件>、MINISQL_BUFFER_FRAMES=2
- 输入：

```sql
SELECT COUNT(*) FROM p;
SELECT COUNT(*) FROM p;
SELECT COUNT(*) FROM p;
SELECT SUM(id) FROM p;
SELECT COUNT(*) FROM p;
```

- 预期结果：小缓存下产生淘汰，日志每行含 policy=LRU / page / generation / dirty / writeBack
- 实测结果：淘汰日志共 1 行；首行：1789457587747 seq=4 policy=LRU page=2 generation=1 dirty=0 writeBack=not-needed
- 结果：✅

**用例 2　缓存命中 / 未命中统计**

- 演示入口：statistics 模式（读统计）
- 输入：

```text
(读统计)
```

- 预期结果：返回 capacity / hits / misses / hitRate / residentPages
- 实测结果：{"capacity": 64, "hitRate": 0.0, "hits": 0, "misses": 4, "residentPages": 4}
- 结果：✅

**用例 3　替换策略契约（固定序列逐项核对）**

- 演示入口：minisql_buffer_contract.exe（直接运行）
- 输入：

```text
(直接运行该程序)
```

- 预期结果：缓冲池契约全部通过
- 实测结果：Evidence: tests/artifacts\buffer-74482528279300
- 结果：✅

#### 2.3 接口与集成（4分）

> 评分表要求：统一接口（get_page / write_page 等）基本功能；与上层模块的衔接

**用例 1　存储层统一接口契约**

- 演示入口：minisql_heap_catalog_contract.exe（直接运行）
- 输入：

```text
(直接运行该程序)
```

- 预期结果：堆存储与目录接口契约通过
- 实测结果：Evidence: tests/artifacts\heap-74482640795100
- 结果：✅

**用例 2　B+ 树索引接口契约**

- 演示入口：minisql_bplus_tree_contract.exe（直接运行）
- 输入：

```text
(直接运行该程序)
```

- 预期结果：B+ 树插入 / 查找 / 范围 / 删除契约通过
- 实测结果：163 B+ tree checks passed
- 结果：✅

**用例 3　上层衔接：一块脚本经缓冲池取页后返回结果**

- 演示入口：execute 模式（限制缓冲池帧数）
- 输入：

```sql
SELECT COUNT(*) FROM p;
SELECT id FROM p WHERE id = 1;
SELECT COUNT(*) FROM p;
```

- 预期结果：三条查询均正确返回
- 实测结果：返回：2 | 1 | 2
- 结果：✅

#### 3.1 执行引擎（4分）

> 评分表要求：使用预设 SQL 检查 SeqScan、Filter、Insert 等核心算子的执行结果

**用例 1　一块脚本连续执行 Insert / SeqScan+Filter / Update / Delete**

- 演示入口：execute 模式（整段脚本 7 条语句）
- 输入：

```sql
CREATE TABLE e(id INT, v INT);
INSERT INTO e VALUES(1,10),(2,20),(3,30);
SELECT id FROM e WHERE v > 15 ORDER BY id;
UPDATE e SET v = v + 1 WHERE id = 1;
SELECT v FROM e ORDER BY id;
DELETE FROM e WHERE id = 3;
SELECT id FROM e ORDER BY id;
```

- 预期结果：依次：建表 → 插入 3 行 → 过滤出 [[2],[3]] → 更新 → v 首值变 11 → 删除 → 剩 [[1],[2]]
- 实测结果：success=True，statements=7；结果依次：(写操作) | (写操作) | 2, 3 | (写操作) | 11, 20, 30 | (写操作) | 1, 2
- 结果：✅

#### 3.2 存储引擎与目录（4分）

> 评分表要求：记录与页面的存取、序列化与反序列化、数据持久化、系统目录管理

**用例 1　序列化与反序列化：CHECK 表达式落盘后重启仍生效**

- 演示入口：execute 模式（跨进程重启）
- 输入：

```sql
CREATE TABLE ck(id INT, n INT, CHECK (n > 0));
INSERT INTO ck VALUES(1, 5);
INSERT INTO ck VALUES(2, -1);
```

- 预期结果：第 3 条被 CHECK 拒绝（说明约束已正确序列化并重建）
- 实测结果：success=False；错误=CHECK constraint failed
- 结果：✅

**用例 2　系统目录：索引登记**

- 演示入口：execute 模式 + catalog 模式
- 输入：

```sql
CREATE INDEX ix_e_v ON e(v);
```

- 预期结果：索引建好后出现在 catalog 的表定义里
- 实测结果：catalog 中表 e 的索引：['ix_e_v']
- 结果：✅

**用例 3　记录存取（读回写入的行）**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id, v FROM e ORDER BY id;
```

- 预期结果：读回 [[1,11],[2,20]]
- 实测结果：[1, 11], [2, 20]
- 结果：✅

#### 3.3 系统集成（4分）

> 评分表要求：通过 CLI 执行「SQL 输入 → 编译 → 执行 → 存储 → 结果返回」完整流程

**用例 1　一块脚本走完整链路，含聚合、分组、排序、过滤**

- 演示入口：execute 模式（整段脚本 5 条语句）
- 输入：

```sql
CREATE TABLE m(id INT, g VARCHAR, n INT);
INSERT INTO m VALUES(1,'a',10),(2,'b',20),(3,'a',30);
SELECT COUNT(*) FROM m;
SELECT g, SUM(n) FROM m GROUP BY g ORDER BY g;
SELECT id FROM m WHERE n >= 20 ORDER BY id;
```

- 预期结果：COUNT=3；分组聚合 [[a,40],[b,20]]；过滤 [[2],[3]]
- 实测结果：success=True，statements=5；结果依次：(写操作) | (写操作) | 3 | ['a', 40], ['b', 20] | 2, 3
- 结果：✅

### 边界与异常情况（对应评分表 二.1「是否处理主要边界情况和异常情况」）

**用例 1　一块脚本覆盖 API 边界与异常：VARCHAR 超长、除零**

- 演示入口：execute 模式（整段脚本）
- 输入：

```sql
CREATE TABLE b(id INT, v VARCHAR(3));
INSERT INTO b VALUES(1, 'ab');
INSERT INTO b VALUES(2, 'abcdef');
SELECT id FROM b WHERE id = 1/0;
SELECT id FROM b WHERE id = 1;
```

- 预期结果：超长被拒（VARCHAR(3) 上限）；除零报错而不是静默返回错值
- 实测结果：success=False；错误=VARCHAR value exceeds Unicode code point limit
- 结果：✅

**用例 2　事务边界：回滚后未提交数据不可见 + 嵌套事务被拒**

- 演示入口：execute 模式（整段脚本）
- 输入：

```sql
BEGIN;
INSERT INTO b VALUES(8, 'yy');
ROLLBACK;
SELECT id FROM b ORDER BY id;
BEGIN;
BEGIN;
```

- 预期结果：回滚后只剩 [[1]]；随后嵌套 BEGIN 被拒
- 实测结果：回滚后结果=；嵌套事务错误=Nested transactions are not supported
- 结果：✅

### 亮点模块（按个人评分，10分）

> 评分表要求：亮点模块是否真正实现并可现场运行；性能类亮点需用优化前后对比证明实际效果

**用例 1　有序索引扫描：排序键为非空索引列时省掉排序**

- 演示入口：对比实验：环境变量 MINISQL_DISABLE_RULES=index-order-scan
- 输入：

```sql
SELECT id FROM big ORDER BY nn;
```

- 预期结果：开启优化后明显快于关闭优化时
- 实测结果：开启 94.1 ms / 关闭 181.3 ms（提速 1.93 倍）
- 结果：✅

**用例 2　Top-N 排序下推：ORDER BY + LIMIT 只保留前 N 行**

- 演示入口：对比实验：环境变量 MINISQL_DISABLE_RULES=top-n-sort,index-order-scan
- 输入：

```sql
SELECT id FROM big ORDER BY nn LIMIT 5;
```

- 预期结果：开启优化后明显快于关闭优化时
- 实测结果：开启 58.9 ms / 关闭 146.5 ms（提速 2.49 倍）
- 结果：✅

**用例 3　查询结果缓存：重复 SELECT 直接命中**

- 演示入口：对比实验：环境变量 MINISQL_RESULT_CACHE=0（同一脚本连跑 5 次）
- 输入：

```sql
SELECT COUNT(*) FROM big WHERE nn > 40;
SELECT COUNT(*) FROM big WHERE nn > 40;
SELECT COUNT(*) FROM big WHERE nn > 40;
SELECT COUNT(*) FROM big WHERE nn > 40;
SELECT COUNT(*) FROM big WHERE nn > 40;
```

- 预期结果：开启缓存后明显快于关闭缓存时
- 实测结果：开启 237.7 ms / 关闭 1008.4 ms（提速 4.24 倍）
- 结果：✅

**用例 4　索引建议器：按实际查询负载给出建索引建议**

- 演示入口：先跑带谓词查询，再读 indexAdvisor 模式
- 输入：

```sql
SELECT id FROM w WHERE v = 1;
SELECT id FROM w WHERE v = 2;
SELECT id FROM w WHERE v = 3;
SELECT id FROM w WHERE k > 1;
```

- 预期结果：按频次排序给出建议，并附可直接执行的 CREATE INDEX 语句
- 实测结果：建议：[["v", 3, "CREATE INDEX idx_w_v ON w(v);"], ["k", 1, "CREATE INDEX idx_w_k ON w(k);"]]
- 结果：✅

**用例 5　多级缓存可观测：statistics 暴露各级缓存命中**

- 演示入口：statistics 模式（读统计）
- 输入：

```text
(读统计)
```

- 预期结果：含 rowCount / liveStats / queryResult / bufferPool 四组计数
- 实测结果：{"rowCount": {"entries": 0, "hits": 0, "misses": 0, "scope": "table-row-counts"}, "liveStats": {"cached": true, "hits": 0, "misses": 1, "scope": "table-column-histograms"}, "queryResult": {"enabled": true, "entries": 0, "hits": 0, "maxRows": 1000, "misses": 0, "scope": "single-statement-select-autocommit"}, "bufferPool": {"capacity": 64, "hitRate": 0.0026857654431512983, "hits": 3, "misses": 1114, …（已截断）
- 结果：✅

### 未通过项

无，全部通过。
