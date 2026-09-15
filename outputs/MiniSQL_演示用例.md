# MiniSQL 演示用例集

对照《大型平台软件设计实习》评分表编排。每条用例都**真跑引擎**，实测结果取自引擎输出，可直接在验收现场按评分项顺序演示。

## 使用说明

- 引擎：`outputs/minisql-backend/bin/minisql_database.exe`
- 词法入口：`outputs/minisql-backend/bin/minisql_lexer.exe`，SQL 由标准输入传入
- 执行模式：`minisql_database.exe <db.pages> <模式>`，模式可选 execute、compile、diagnostics、catalog、statistics、indexAdvisor
- 用例总数：**46**，通过 **46**，未通过 **0**

优化前后对比所用的环境变量：

- `MINISQL_DISABLE_RULES=<规则名>[,<规则名>]`：关闭指定优化规则，规则名同 EXPLAIN 输出里的 ruleId
- `MINISQL_NO_OPTIMIZE=1`：完全关闭优化器
- `MINISQL_RESULT_CACHE=0`：关闭查询结果缓存
- `MINISQL_BUFFER_LOG=<文件>`：输出缓冲池淘汰日志
- `MINISQL_BUFFER_FRAMES=<n>`：设置缓冲池帧数

## 一、基本功能完成情况（40分）

### 1、SQL 编译器（16分）

#### 1.1 词法分析（4分）

> 评分表要求：关键字、标识符、常量、运算符等 Token 识别是否正确；非法输入处理

**用例 1.1-1　关键字 / 标识符 / 运算符 / 常量 Token 识别**

- 演示入口：minisql_lexer.exe（标准输入），compile 模式可用
- 输入：

```sql
SELECT id FROM t WHERE v >= 10;
```

- 预期结果：SELECT / FROM / WHERE = KEYWORD；id / t / v = IDENTIFIER；>= = OPERATOR；10 = INTEGER
- 实测结果：[["KEYWORD", "SELECT"], ["IDENTIFIER", "id"], ["KEYWORD", "FROM"], ["IDENTIFIER", "t"], ["KEYWORD", "WHERE"], ["IDENTIFIER", "v"], ["OPERATOR", ">="], ["INTEGER", "10"], ["DELIMITER", ";"]]
- 结果：✅

**用例 1.1-2　字符串常量与分隔符**

- 演示入口：minisql_lexer.exe
- 输入：

```sql
INSERT INTO t VALUES(4,'x y');
```

- 预期结果：'x y' = STRING（含空格不切分）；; = DELIMITER
- 实测结果：[["KEYWORD", "INSERT"], ["KEYWORD", "INTO"], ["IDENTIFIER", "t"], ["KEYWORD", "VALUES"], ["DELIMITER", "("], ["INTEGER", "4"], ["DELIMITER", ","], ["STRING", "'x y'"], ["DELIMITER", ")"], ["DELIMITER"…
- 结果：✅

**用例 1.1-3　注释被正确跳过**

- 演示入口：minisql_lexer.exe
- 输入：

```sql
-- 注释
SELECT 1;
```

- 预期结果：注释不产生 Token，只剩 SELECT / 1 / ;
- 实测结果：[["KEYWORD", "SELECT"], ["INTEGER", "1"], ["DELIMITER", ";"]]
- 结果：✅

**用例 1.1-4　非法输入处理**

- 演示入口：minisql_lexer.exe
- 输入：

```sql
SELECT id FROM t WHERE v = @;
```

- 预期结果：LexicalError，并给出 line / column 定位
- 实测结果：Illegal character（line=1, column=28）
- 结果：✅

#### 1.2 语法分析（4分）

> 评分表要求：AST 构建是否正确，至少支持 4 类 SQL 语句；典型语法错误能否被识别

**用例 1.2-1　AST 构建（评分表要求至少 4 类语句）**

- 演示入口：compile 模式
- 输入：

```sql
CREATE TABLE g(a INT);
INSERT INTO t VALUES(9,90,'z');
SELECT id FROM t;
UPDATE t SET v=11 WHERE id=1;
DELETE FROM t WHERE id=3;
```

- 预期结果：五条语句的 ast.kind 依次为 CreateTable / Insert / Select / Update / Delete
- 实测结果：{"CREATE TABLE": "CreateTable", "INSERT": "Insert", "SELECT": "Select", "UPDATE": "Update", "DELETE": "Delete"}
- 结果：✅

**用例 1.2-2　典型语法错误识别（缺操作数）**

- 演示入口：compile 模式
- 输入：

```sql
SELECT id FROM t WHERE v > ;
```

- 预期结果：SyntaxError，并给出 line / column
- 实测结果：Expected identifier, literal or '('（line=1, column=28）
- 结果：✅

**用例 1.2-3　缺少必需成分**

- 演示入口：compile 模式
- 输入：

```sql
SELECT FROM;
```

- 预期结果：SyntaxError：缺少投影列
- 实测结果：Expected identifier, literal or '('
- 结果：✅

**用例 1.2-4　错误恢复（一条错不影响后续）**

- 演示入口：diagnostics 模式
- 输入：

```sql
SELECT FROM; SELECT id FROM t;
```

- 预期结果：第一条报 parser 阶段错误，第二条仍被解析
- 实测结果：[["parser", "Expected identifier, literal or '('"]]
- 结果：✅

#### 1.3 语义分析（4分）

> 评分表要求：表 / 列存在性、数据类型、列数匹配等语义检查；Catalog 使用是否合理

**用例 1.3-1　表存在性检查**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id FROM nope;
```

- 预期结果：SemanticError：表不存在
- 实测结果：SemanticError：Table does not exist: nope
- 结果：✅

**用例 1.3-2　列存在性检查**

- 演示入口：execute 模式
- 输入：

```sql
SELECT nope FROM t;
```

- 预期结果：SemanticError：列不存在
- 实测结果：SemanticError：Column 'nope' does not exist in table 't'
- 结果：✅

**用例 1.3-3　数据类型检查**

- 演示入口：execute 模式
- 输入：

```sql
INSERT INTO t VALUES('abc',1,'x');
```

- 预期结果：SemanticError：INT 列收到字符串
- 实测结果：SemanticError：INSERT type mismatch: t.id expects INT, but VARCHAR found
- 结果：✅

**用例 1.3-4　列数匹配检查**

- 演示入口：execute 模式
- 输入：

```sql
INSERT INTO t VALUES(1);
```

- 预期结果：SemanticError：列数与值数不一致
- 实测结果：SemanticError：INSERT column/value count mismatch
- 结果：✅

**用例 1.3-5　Catalog 使用（表 / 列元数据）**

- 演示入口：catalog 模式（不执行 SQL）
- 输入：（读系统目录）
- 预期结果：返回表 t 的列名与类型
- 实测结果：{"table": "t", "columns": [["id", "int"], ["v", "int"], ["name", "varchar"]]}
- 结果：✅

#### 1.4 执行计划生成（4分）

> 评分表要求：能否正确生成树形 / JSON 逻辑执行计划；算子及其组织关系是否正确

**用例 1.4-1　逻辑执行计划（JSON）**

- 演示入口：compile 模式
- 输入：

```sql
SELECT id FROM t WHERE v > 10;
```

- 预期结果：Project → Filter → SeqScan 的算子树
- 实测结果：["Project", "Filter", "SeqScan"]
- 结果：✅

**用例 1.4-2　连接算子组织关系**

- 演示入口：execute 模式（EXPLAIN）
- 输入：

```sql
EXPLAIN SELECT t.id FROM t JOIN t AS t2 ON t.id=t2.id;
```

- 预期结果：计划含 Join 类算子
- 实测结果：["Project", "HashJoin", "SeqScan", "SeqScan"]
- 结果：✅

**用例 1.4-3　聚合算子组织关系**

- 演示入口：execute 模式（EXPLAIN）
- 输入：

```sql
EXPLAIN SELECT v, COUNT(*) FROM t GROUP BY v;
```

- 预期结果：计划含 Aggregate 算子
- 实测结果：["Project", "Aggregate", "SeqScan"]
- 结果：✅

**用例 1.4-4　优化前后计划对照**

- 演示入口：execute 模式（EXPLAIN）
- 输入：

```sql
EXPLAIN SELECT id FROM t WHERE 1=1;
```

- 预期结果：同时给出原始计划与优化后计划，并列出命中的优化规则
- 实测结果：optimizedPlan = ['Project', 'SeqScan']；optimizationRules = ['constant-comparison', 'remove-true-filter', 'prune-columns']
- 结果：✅

### 2、存储系统（12分）

#### 2.1 页式存储管理（4分）

> 评分表要求：页面分配、释放、读取、写入以及数据恢复是否正确

**用例 2.1-1　页分配 / 释放 / 读取 / 写入**

- 演示入口：minisql_page_bplus_tree_contract.exe
- 输入：（直接运行该程序）
- 预期结果：页级 B+ 树全量检查通过
- 实测结果：542 page-level B+ tree checks passed
- 结果：✅

**用例 2.1-2　页文件与堆存储契约**

- 演示入口：minisql_storage_contract.exe
- 输入：（直接运行该程序）
- 预期结果：存储契约检查通过
- 实测结果：Evidence: tests/artifacts\storage-73839874900000
- 结果：✅

**用例 2.1-3　数据恢复（写入后新进程读取）**

- 演示入口：execute 模式（第二次启动进程）
- 输入：

```sql
SELECT id,v FROM p ORDER BY id;
```

- 预期结果：[[1,"a"],[2,"b"]]（数据已落盘）
- 实测结果：[[1, "a"], [2, "b"]]
- 结果：✅

#### 2.2 缓存机制（4分）

> 评分表要求：固定页面访问序列下，缓存命中、淘汰、替换和回写是否符合 LRU 策略

**用例 2.2-1　缓存命中 / 未命中统计**

- 演示入口：statistics 模式（不执行 SQL）
- 输入：（读统计）
- 预期结果：返回 capacity / hits / misses / hitRate / residentPages
- 实测结果：{"capacity": 64, "hitRate": 0.0, "hits": 0, "misses": 4, "residentPages": 4}
- 结果：✅

**用例 2.2-2　LRU 淘汰 + 替换 + 回写日志**

- 演示入口：环境变量 MINISQL_BUFFER_LOG=<文件>、MINISQL_BUFFER_FRAMES=2
- 输入：SELECT COUNT(*) FROM p;（小缓存下连跑 3 次）
- 预期结果：日志每行含 policy=LRU / page / generation / dirty / writeBack
- 实测结果：1789456945496 seq=4 policy=LRU page=2 generation=1 dirty=0 writeBack=not-needed
- 结果：✅

**用例 2.2-3　固定访问序列下的替换正确性**

- 演示入口：minisql_buffer_contract.exe
- 输入：（直接运行该程序）
- 预期结果：缓冲池契约（命中 / 淘汰 / 策略）全部通过
- 实测结果：Evidence: tests/artifacts\buffer-73840222282100
- 结果：✅

#### 2.3 接口与集成（4分）

> 评分表要求：统一接口（get_page / write_page 等）基本功能；与上层模块的衔接

**用例 2.3-1　存储层统一接口契约**

- 演示入口：minisql_heap_catalog_contract.exe
- 输入：（直接运行该程序）
- 预期结果：堆存储与目录接口契约通过
- 实测结果：Evidence: tests/artifacts\heap-73840285577300
- 结果：✅

**用例 2.3-2　B+ 树索引接口契约**

- 演示入口：minisql_bplus_tree_contract.exe
- 输入：（直接运行该程序）
- 预期结果：B+ 树插入 / 查找 / 范围 / 删除契约通过
- 实测结果：163 B+ tree checks passed
- 结果：✅

**用例 2.3-3　与上层衔接（SQL 经缓冲池取页）**

- 演示入口：execute 模式，限制缓冲池帧数
- 输入：

```sql
SELECT COUNT(*) FROM p;
```

- 预期结果：查询仍正确返回 2
- 实测结果：[[2]]
- 结果：✅

### 3、数据库系统（12分）

#### 3.1 执行引擎（4分）

> 评分表要求：使用预设 SQL 检查 SeqScan、Filter、Insert 等核心算子的执行结果

**用例 3.1-1　Insert 算子**

- 演示入口：execute 模式
- 输入：

```sql
CREATE TABLE e(id INT, v INT);
INSERT INTO e VALUES(1,10),(2,20),(3,30);
```

- 预期结果：success = true
- 实测结果：success = True
- 结果：✅

**用例 3.1-2　SeqScan + Filter 算子**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id FROM e WHERE v > 15 ORDER BY id;
```

- 预期结果：[[2],[3]]
- 实测结果：[[2], [3]]
- 结果：✅

**用例 3.1-3　Update 算子**

- 演示入口：execute 模式
- 输入：

```sql
UPDATE e SET v = v + 1 WHERE id = 1;
SELECT v FROM e ORDER BY id;
```

- 预期结果：第一条由 10 变为 11
- 实测结果：[[11], [20], [30]]
- 结果：✅

**用例 3.1-4　Delete 算子**

- 演示入口：execute 模式
- 输入：

```sql
DELETE FROM e WHERE id = 3;
SELECT id FROM e ORDER BY id;
```

- 预期结果：只剩 [[1],[2]]
- 实测结果：[[1], [2]]
- 结果：✅

#### 3.2 存储引擎与目录（4分）

> 评分表要求：记录与页面的存取、序列化与反序列化、数据持久化、系统目录管理

**用例 3.2-1　记录存取（读回写入的行）**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id, v FROM e ORDER BY id;
```

- 预期结果：读回 [[1,11],[2,20]]
- 实测结果：[[1, 11], [2, 20]]
- 结果：✅

**用例 3.2-2　序列化与反序列化（CHECK 约束落盘后仍生效）**

- 演示入口：execute 模式（跨进程重启）
- 输入：

```sql
CREATE TABLE ck(id INT, n INT, CHECK (n > 0));
INSERT INTO ck VALUES(1, 5);
INSERT INTO ck VALUES(2, -1);
```

- 预期结果：重启后 CHECK 仍拒绝 n = -1
- 实测结果：ExecutionError：CHECK constraint failed
- 结果：✅

**用例 3.2-3　索引创建与落盘**

- 演示入口：execute 模式
- 输入：

```sql
CREATE INDEX ix_e_v ON e(v);
```

- 预期结果：success = true，索引登记进目录
- 实测结果：success = True
- 结果：✅

**用例 3.2-4　系统目录管理**

- 演示入口：catalog 模式（不执行 SQL）
- 输入：（读系统目录）
- 预期结果：表 e 的定义里含索引 ix_e_v
- 实测结果：{"table": "e", "indexes": ["ix_e_v"]}
- 结果：✅

#### 3.3 系统集成（4分）

> 评分表要求：通过 CLI 执行「SQL 输入 → 编译 → 执行 → 存储 → 结果返回」完整流程

**用例 3.3-1　CLI 完整链路：SQL 输入→编译→执行→存储→结果返回**

- 演示入口：execute 模式（一次多语句脚本）
- 输入：

```sql
CREATE TABLE s(a INT);
INSERT INTO s VALUES(1),(2);
SELECT COUNT(*) FROM s;
```

- 预期结果：statements = 3，COUNT = 2
- 实测结果：statements = 3，rows = [[2]]
- 结果：✅

**用例 3.3-2　完整流程含聚合与排序**

- 演示入口：execute 模式（多语句脚本）
- 输入：

```sql
CREATE TABLE m(id INT, g VARCHAR, n INT);
INSERT INTO m VALUES(1,'a',10),(2,'b',20),(3,'a',30);
SELECT g, SUM(n) FROM m GROUP BY g ORDER BY g;
```

- 预期结果：[['a', 40], ['b', 20]]
- 实测结果：[["a", 40], ["b", 20]]
- 结果：✅

### 边界与异常情况（对应评分表 二.1「是否处理主要边界情况和异常情况」）

**用例 边界与异常情况-1　VARCHAR 超长（边界）**

- 演示入口：execute 模式
- 输入：

```sql
INSERT INTO b VALUES(2, 'abcdef');
```

- 预期结果：拒绝：超过 VARCHAR(3) 上限
- 实测结果：VARCHAR value exceeds Unicode code point limit
- 结果：✅

**用例 边界与异常情况-2　空表达式（异常）**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id FROM b WHERE id = ;
```

- 预期结果：SyntaxError
- 实测结果：Expected identifier, literal or '('
- 结果：✅

**用例 边界与异常情况-3　除零（异常）**

- 演示入口：execute 模式
- 输入：

```sql
SELECT id FROM b WHERE id = 1/0;
```

- 预期结果：报错，而不是静默返回错误结果
- 实测结果：ExecutionError：Division by zero
- 结果：✅

**用例 边界与异常情况-4　嵌套事务（异常）**

- 演示入口：execute 模式
- 输入：

```sql
BEGIN;
INSERT INTO b VALUES(9, 'zz');
BEGIN;
COMMIT;
```

- 预期结果：拒绝嵌套事务
- 实测结果：Nested transactions are not supported
- 结果：✅

**用例 边界与异常情况-5　回滚边界（未提交数据不落盘）**

- 演示入口：execute 模式
- 输入：

```sql
BEGIN;
INSERT INTO b VALUES(8, 'yy');
ROLLBACK;
SELECT id FROM b ORDER BY id;
```

- 预期结果：只剩 [[1]]，回滚后新值不可见
- 实测结果：[[1]]
- 结果：✅

## 三、项目质量与创新性（10分）

### 亮点模块（按个人评分，10分）

> 评分表要求：亮点模块是否真正实现并可现场运行；性能类亮点需用优化前后对比证明实际效果

**用例 亮点模块-1　有序索引扫描：排序键为非空索引列时省掉排序**

- 演示入口：对比实验：环境变量 MINISQL_DISABLE_RULES=index-order-scan
- 输入：

```sql
SELECT id FROM big ORDER BY nn;
```

- 预期结果：开启优化后明显快于关闭优化时
- 实测结果：开启 83.9 ms / 关闭 189.9 ms（提速 2.26 倍）
- 结果：✅

**用例 亮点模块-2　Top-N 排序下推：ORDER BY + LIMIT 只保留前 N 行**

- 演示入口：对比实验：环境变量 MINISQL_DISABLE_RULES=top-n-sort,index-order-scan
- 输入：

```sql
SELECT id FROM big ORDER BY nn LIMIT 5;
```

- 预期结果：开启优化后明显快于关闭优化时
- 实测结果：开启 58.8 ms / 关闭 149.8 ms（提速 2.55 倍）
- 结果：✅

**用例 亮点模块-3　查询结果缓存：重复 SELECT 直接命中**

- 演示入口：对比实验：环境变量 MINISQL_RESULT_CACHE=0（同进程连跑 5 次）
- 输入：

```sql
SELECT COUNT(*) FROM big WHERE nn > 40;
```

- 预期结果：开启缓存后明显快于关闭缓存时
- 实测结果：开启 229.0 ms / 关闭 966.5 ms（提速 4.22 倍）
- 结果：✅

**用例 亮点模块-4　索引建议器：按实际查询负载给出建索引建议**

- 演示入口：先跑带谓词查询，再读 indexAdvisor 模式
- 输入：SELECT id FROM w WHERE v=1;
SELECT id FROM w WHERE v=2;
SELECT id FROM w WHERE v=3;
SELECT id FROM w WHERE k>1;
- 预期结果：按频次排序给出建议，并附可直接执行的 CREATE INDEX 语句
- 实测结果：[["v", 3, "CREATE INDEX idx_w_v ON w(v);"], ["k", 1, "CREATE INDEX idx_w_k ON w(k);"]]
- 结果：✅

**用例 亮点模块-5　多级缓存可观测：statistics 暴露各级缓存命中**

- 演示入口：statistics 模式（不执行 SQL）
- 输入：（读统计）
- 预期结果：含 rowCount / liveStats / queryResult / bufferPool 四组计数
- 实测结果：{"rowCount": {"entries": 0, "hits": 0, "misses": 0, "scope": "table-row-counts"}, "liveStats": {"cached": true, "hits": 0, "misses": 1, "scope": "table-column-histograms"}, "queryResult": {"enabled": …
- 结果：✅

### 未通过用例

无，全部用例通过。
