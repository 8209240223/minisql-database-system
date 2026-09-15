# MiniSQL 演示用例集

对照《大型平台软件设计实习》评分表编排。验收前预先设计测试用例与预期结果，验收时可直接按评分项顺序运行。
每条用例都**真跑引擎**，实测结果取自引擎输出。

| 项 | 值 |
| --- | --- |
| 引擎 | `outputs/minisql-backend/bin/minisql_database.exe` |
| 词法入口 | `outputs/minisql-backend/bin/minisql_lexer.exe`（SQL 由标准输入传入） |
| 调用方式 | `minisql_database.exe <db.pages> <模式>`，模式可选：execute / compile / diagnostics / catalog / statistics / indexAdvisor |
| 用例总数 | 46 |
| 通过 | 46 |
| 未通过 | 0 |

优化前后对比所用的环境开关：

| 开关 | 作用 |
| --- | --- |
| `MINISQL_DISABLE_RULES=<规则名>[,<规则名>]` | 关闭指定优化规则，规则名同 EXPLAIN 输出里的 ruleId |
| `MINISQL_NO_OPTIMIZE=1` | 完全关闭优化器 |
| `MINISQL_RESULT_CACHE=0` | 关闭查询结果缓存 |
| `MINISQL_BUFFER_LOG=<文件>` | 输出缓冲池淘汰日志 |
| `MINISQL_BUFFER_FRAMES=<n>` | 设置缓冲池帧数 |

## 一、基本功能完成情况（40分）

### 1、SQL 编译器（16分）

#### 1.1 词法分析

**分值**：4分　　　**评分表要求**：关键字、标识符、常量、运算符等 Token 识别是否正确；非法输入处理

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 关键字 / 标识符 / 运算符 / 常量 Token 识别 | minisql_lexer.exe | `SELECT id FROM t WHERE v >= 10;` | SELECT/FROM/WHERE = KEYWORD；id/t/v = IDENTIFIER；>= = OPERATOR；10 = INTEGER | [["KEYWORD", "SELECT"], ["IDENTIFIER", "id"], ["KEYWORD", "FROM"], ["IDENTIFIER", "t"], ["KEYWORD", "WHERE"], ["IDENTIFIER", "v"],… | ✅ |
| 2 | 字符串常量与分隔符 | minisql_lexer.exe | `INSERT INTO t VALUES(4,'x y');` | 'x y' = STRING（含空格不切分）；; = DELIMITER | [["KEYWORD", "INSERT"], ["KEYWORD", "INTO"], ["IDENTIFIER", "t"], ["KEYWORD", "VALUES"], ["DELIMITER", "("], ["INTEGER", "4"], ["D… | ✅ |
| 3 | 注释被正确跳过 | minisql_lexer.exe | `-- 注释\nSELECT 1;` | 注释不产生 Token，只剩 SELECT / 1 / ; | [["KEYWORD", "SELECT"], ["INTEGER", "1"], ["DELIMITER", ";"]] | ✅ |
| 4 | 非法输入处理 | minisql_lexer.exe | `SELECT id FROM t WHERE v = @;` | LexicalError，并给出 line / column 定位 | Illegal character @line=1 col=28 | ✅ |

#### 1.2 语法分析

**分值**：4分　　　**评分表要求**：AST 构建是否正确，至少支持 4 类 SQL 语句；典型语法错误能否被识别

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | AST 构建（评分要求至少 4 类 SQL 语句） | compile 模式 | `CREATE TABLE / INSERT / SELECT / UPDATE / DELETE 各一条` | ast.kind = CreateTable / Insert / Select / Update / Delete | {"CREATE TABLE": "CreateTable", "INSERT": "Insert", "SELECT": "Select", "UPDATE": "Update", "DELETE": "Delete"} | ✅ |
| 2 | 典型语法错误识别 | compile 模式 | `SELECT id FROM t WHERE v > ;` | SyntaxError，并给出 line / column | Expected identifier, literal or '(' @line=1 col=28 | ✅ |
| 3 | 缺少必需成分 | compile 模式 | `SELECT FROM;` | SyntaxError：缺少投影列 | Expected identifier, literal or '(' | ✅ |
| 4 | 错误恢复（一条错不影响后续） | diagnostics 模式 | `SELECT FROM; SELECT id FROM t;` | 第一条报 parser 错误，第二条仍被解析 | [["parser", "Expected identifier, literal or '('"]] | ✅ |

#### 1.3 语义分析

**分值**：4分　　　**评分表要求**：表 / 列存在性、数据类型、列数匹配等语义检查；Catalog 使用是否合理

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 表存在性检查 | execute 模式 | `SELECT id FROM nope;` | SemanticError：表不存在 | SemanticError: Table does not exist: nope | ✅ |
| 2 | 列存在性检查 | execute 模式 | `SELECT nope FROM t;` | SemanticError：列不存在 | SemanticError: Column 'nope' does not exist in table 't' | ✅ |
| 3 | 数据类型检查 | execute 模式 | `INSERT INTO t VALUES('abc',1,'x');` | SemanticError：INT 列收到字符串 | SemanticError: INSERT type mismatch: t.id expects INT, but VARCHAR found | ✅ |
| 4 | 列数匹配检查 | execute 模式 | `INSERT INTO t VALUES(1);` | SemanticError：列数与值数不一致 | SemanticError: INSERT column/value count mismatch | ✅ |
| 5 | Catalog 使用（表 / 列元数据） | catalog 模式 | `（读系统目录）` | 返回表 t 的列名与类型 | {"table": "t", "columns": [["id", "int"], ["v", "int"], ["name", "varchar"]]} | ✅ |

#### 1.4 执行计划生成

**分值**：4分　　　**评分表要求**：能否正确生成树形 / JSON 逻辑执行计划；算子及其组织关系是否正确

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 逻辑执行计划（JSON） | compile 模式 | `SELECT id FROM t WHERE v > 10;` | Project → Filter → SeqScan | ["Project", "Filter", "SeqScan"] | ✅ |
| 2 | 连接算子组织关系 | EXPLAIN | `EXPLAIN SELECT t.id FROM t JOIN t AS t2 ON t.id=t2.id;` | 计划含 Join 类算子 | ["Project", "HashJoin", "SeqScan", "SeqScan"] | ✅ |
| 3 | 聚合算子组织关系 | EXPLAIN | `EXPLAIN SELECT v, COUNT(*) FROM t GROUP BY v;` | 计划含 Aggregate 算子 | ["Project", "Aggregate", "SeqScan"] | ✅ |
| 4 | 优化前后计划对照 | EXPLAIN | `EXPLAIN SELECT id FROM t WHERE 1=1;` | 同时给出 plan 与 optimizedPlan，并列出命中的优化规则 | optimized=['Project', 'SeqScan'], rules=['constant-comparison', 'remove-true-filter', 'prune-columns'] | ✅ |

### 2、存储系统（12分）

#### 2.1 页式存储管理

**分值**：4分　　　**评分表要求**：页面分配、释放、读取、写入以及数据恢复是否正确

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 页分配 / 释放 / 读取 / 写入 | minisql_page_bplus_tree_contract.exe | `（直接运行）` | 页级 B+ 树全量检查通过 | 542 page-level B+ tree checks passed | ✅ |
| 2 | 页文件与堆存储契约 | minisql_storage_contract.exe | `（直接运行）` | 存储契约检查通过 | Evidence: tests/artifacts\storage-73666223657400 | ✅ |
| 3 | 数据恢复（写入后新进程读取） | execute 模式（第二次启动进程） | `SELECT id,v FROM p ORDER BY id;` | [[1,"a"],[2,"b"]]（数据已落盘） | [[1, "a"], [2, "b"]] | ✅ |

#### 2.2 缓存机制

**分值**：4分　　　**评分表要求**：固定页面访问序列下，缓存命中、淘汰、替换和回写是否符合 LRU 策略

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 缓存命中 / 未命中统计 | statistics 模式 | `（读统计）` | 返回 capacity / hits / misses / hitRate / residentPages | {"capacity": 64, "hitRate": 0.0, "hits": 0, "misses": 4, "residentPages": 4} | ✅ |
| 2 | LRU 淘汰 + 替换 + 回写日志 | MINISQL_BUFFER_LOG=<文件> + MINISQL_BUFFER_FRAMES=2 | `SELECT COUNT(*) FROM p;（小缓存下连跑 3 次）` | 日志每行含 policy=LRU / page / generation / dirty / writeBack | 1789456771787 seq=4 policy=LRU page=2 generation=1 dirty=0 writeBack=not-needed | ✅ |
| 3 | 固定访问序列下的替换正确性 | minisql_buffer_contract.exe | `（直接运行）` | 缓冲池契约（命中 / 淘汰 / 策略）全部通过 | Evidence: tests/artifacts\buffer-73666512231500 | ✅ |

#### 2.3 接口与集成

**分值**：4分　　　**评分表要求**：统一接口（get_page / write_page 等）基本功能；与上层模块的衔接

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 存储层统一接口契约 | minisql_heap_catalog_contract.exe | `（直接运行）` | 堆存储与目录接口契约通过 | Evidence: tests/artifacts\heap-73666589494200 | ✅ |
| 2 | B+ 树索引接口契约 | minisql_bplus_tree_contract.exe | `（直接运行）` | B+ 树插入 / 查找 / 范围 / 删除契约通过 | 163 B+ tree checks passed | ✅ |
| 3 | 与上层衔接（SQL 经缓冲池取页） | execute 模式（限制帧数） | `SELECT COUNT(*) FROM p;` | 查询仍正确返回 2 | [[2]] | ✅ |

### 3、数据库系统（12分）

#### 3.1 执行引擎

**分值**：4分　　　**评分表要求**：使用预设 SQL 检查 SeqScan、Filter、Insert 等核心算子的执行结果

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | Insert 算子 | execute 模式 | `CREATE TABLE e(id INT,v INT); INSERT INTO e VALUES(1,10),(2,20),(3,30);` | success=true | success=True | ✅ |
| 2 | SeqScan + Filter 算子 | execute 模式 | `SELECT id FROM e WHERE v > 15 ORDER BY id;` | [[2],[3]] | [[2], [3]] | ✅ |
| 3 | Update 算子 | execute 模式 | `UPDATE e SET v=v+1 WHERE id=1; SELECT v FROM e ORDER BY id;` | 第一条由 10 变为 11 | [[11], [20], [30]] | ✅ |
| 4 | Delete 算子 | execute 模式 | `DELETE FROM e WHERE id=3; SELECT id FROM e ORDER BY id;` | 只剩 [[1],[2]] | [[1], [2]] | ✅ |

#### 3.2 存储引擎与目录

**分值**：4分　　　**评分表要求**：记录与页面的存取、序列化与反序列化、数据持久化、系统目录管理

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 记录存取（读回写入的行） | execute 模式 | `SELECT id, v FROM e ORDER BY id;` | 读回 [[1,11],[2,20]] | [[1, 11], [2, 20]] | ✅ |
| 2 | 序列化与反序列化（CHECK 约束落盘后仍生效） | execute 模式（跨进程重启） | `CREATE TABLE ck(id INT, n INT, CHECK (n > 0)); INSERT INTO ck VALUES(1,5); 再插 (2,-1);` | 重启后 CHECK 仍拒绝 n=-1 | ExecutionError: CHECK constraint failed | ✅ |
| 3 | 索引创建与落盘 | execute 模式 | `CREATE INDEX ix_e_v ON e(v);` | success=true，索引登记进目录 | success=True | ✅ |
| 4 | 系统目录管理 | catalog 模式 | `（读系统目录）` | 表 e 定义含索引 ix_e_v | {"table": "e", "indexes": ["ix_e_v"]} | ✅ |

#### 3.3 系统集成

**分值**：4分　　　**评分表要求**：通过 CLI 执行「SQL 输入—编译—执行—存储—结果返回」完整流程

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | CLI 完整链路：SQL→编译→执行→存储→结果返回 | execute 模式（一次多语句脚本） | `CREATE TABLE s(a INT); INSERT INTO s VALUES(1),(2); SELECT COUNT(*) FROM s;` | statements=3，COUNT=2 | statements=3, rows=[[2]] | ✅ |
| 2 | 完整流程含聚合与排序 | execute 模式（多语句脚本） | `CREATE TABLE m(...); INSERT 三行; SELECT g,SUM(n) FROM m GROUP BY g ORDER BY g;` | [['a',40],['b',20]] | [["a", 40], ["b", 20]] | ✅ |

### 边界与异常情况（对应评分表 二.1）

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | VARCHAR 超长（边界） | execute 模式 | `INSERT INTO b VALUES(2,'abcdef');` | 拒绝：超过 VARCHAR(3) 上限 | VARCHAR value exceeds Unicode code point limit | ✅ |
| 2 | 空表达式（异常） | execute 模式 | `SELECT id FROM b WHERE id = ;` | SyntaxError | Expected identifier, literal or '(' | ✅ |
| 3 | 除零（异常） | execute 模式 | `SELECT id FROM b WHERE id = 1/0;` | 报错而非静默返回错误结果 | ExecutionError: Division by zero | ✅ |
| 4 | 嵌套事务（异常） | execute 模式 | `BEGIN; INSERT INTO b VALUES(9,'zz'); BEGIN; COMMIT;` | 拒绝嵌套事务 | Nested transactions are not supported | ✅ |
| 5 | 回滚边界（未提交数据不落盘） | execute 模式 | `BEGIN; INSERT INTO b VALUES(8,'yy'); ROLLBACK; SELECT id FROM b ORDER BY id;` | 只剩 [[1]]，回滚后新值不可见 | [[1]] | ✅ |

## 三、项目质量与创新性（10分）

### 亮点模块（按个人评分）

**分值**：10分　　　**评分表要求**：亮点模块是否真正实现并可现场运行；性能类亮点需用优化前后对比证明实际效果

| 编号 | 测什么 | 演示入口 | SQL / 命令 | 预期结果 | 实测结果 | 结果 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 亮点：有序索引扫描（排序键为非空索引列时省掉排序） | 对比实验：MINISQL_DISABLE_RULES=index-order-scan | `SELECT id FROM big ORDER BY nn;` | 开启优化后明显快于关闭时 | 开启 99.4 ms / 关闭 179.2 ms（提速 1.80×） | ✅ |
| 2 | 亮点：Top-N 排序下推（ORDER BY + LIMIT 只保留前 N 行） | 对比实验：MINISQL_DISABLE_RULES=top-n-sort,index-order-scan | `SELECT id FROM big ORDER BY nn LIMIT 5;` | 开启优化后明显快于关闭时 | 开启 62.1 ms / 关闭 153.4 ms（提速 2.47×） | ✅ |
| 3 | 亮点：查询结果缓存（重复 SELECT 直接命中） | 对比实验：MINISQL_RESULT_CACHE=0（同进程连跑 5 次） | `SELECT COUNT(*) FROM big WHERE nn > 40;` | 开启缓存后明显快于关闭时 | 开启 237.3 ms / 关闭 967.4 ms（提速 4.08×） | ✅ |
| 4 | 亮点：索引建议器（按实际查询负载给出建索引建议） | 先跑带谓词查询，再读 indexAdvisor 模式 | `SELECT id FROM w WHERE v=1;（v=2 / v=3 / k>1 各一次）` | 按频次排序给出建议，并附可直接执行的 CREATE INDEX 语句 | [["v", 3, "CREATE INDEX idx_w_v ON w(v);"], ["k", 1, "CREATE INDEX idx_w_k ON w(k);"]] | ✅ |
| 5 | 亮点：多级缓存可观测（statistics 暴露各级缓存命中） | statistics 模式 | `（读统计）` | 含 rowCount / liveStats / queryResult / bufferPool 四组计数 | {"rowCount": {"entries": 0, "hits": 0, "misses": 0, "scope": "table-row-counts"}, "liveStats": {"cached": true, "hits": 0, "misses… | ✅ |

### 未通过用例

无，全部用例通过。
