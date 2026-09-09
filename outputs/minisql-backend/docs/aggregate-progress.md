# 聚合查询实施进展

日期：2026-09-08。对应 EXT-SQL-006 与 X06。COUNT、SUM、整数 AVG、MIN、MAX、GROUP BY 与 HAVING 已可执行；新增类型、外部聚合及完整 X06 验收仍未完成。

## 一、目标契约

完整执行顺序保持 JOIN、WHERE、Aggregate、HAVING、Project、DISTINCT、ORDER BY、LIMIT。投影不能任意读取组内的非分组列；WHERE 与同层嵌套聚合必须由语义阶段拒绝。GROUP BY/HAVING 不采用投影别名替换，ORDER BY 沿用已有别名规则。

COUNT(*) 包含 NULL 行，COUNT(表达式) 忽略 NULL；SUM、AVG、MIN、MAX 忽略 NULL。全局聚合的空输入返回一行，COUNT 为 0，其余 NULL；带分组键的空输入返回零组。COUNT 为 BIGINT，整数 SUM 使用受检查的 BIGINT 累加，整数 AVG 为 DECIMAL(38,6)，不以双精度浮点替代。DECIMAL 表达式及持久化列已接通，具体精度与存储契约见 decimal-progress.md 和 decimal-storage-progress.md。

## 二、本次实现

1. Parser 接受 COUNT、SUM、AVG、MIN、MAX 的单表达式参数，COUNT 额外接受星号。函数名只有后接左括号才识别为函数，仍可作为普通列名；大小写不敏感。
2. AST 使用 AggregateExpr，其 value 为规范化函数名，left 为参数；COUNT(*) 的参数是 Wildcard。函数及参数各自保留源码位置。函数可以出现在算术、CAST 或比较表达式内，嵌套结构也保留供后续语义诊断，不在解析器中假装完成语义检查。
3. Statement 新增 groupBy 表达式序列和 having 表达式。解析顺序为 WHERE、GROUP BY、HAVING、ORDER BY、LIMIT、OFFSET，拒绝空子句、错误顺序、多参数及不闭合括号。嵌套沿用 256 层保护。
4. AST JSON 输出 groupBy 和 having，聚合节点复用递归表达式序列化。没有放宽持久化 CHECK 反序列化器的节点白名单。
5. 语义层已检查聚合上下文与分组约束，合法聚合查询可编译为 Aggregate 计划。五种聚合均可执行，整数 AVG 使用大整数累加后一次 HALF_UP 舍入，输出固定六位十进制字符串；不沿用 SUM 的 BIGINT 总和上限。非法聚合返回 2003 语义错误。
6. 分组表达式按绑定后的列位置和表达式结构比较，表限定名与大小写不同但绑定相同列时可匹配；整数字面量按值规范化。分组键组合及常量表达式可投影，但不任意取组内非分组列。星号逐列检查，只有全部展开列都满足分组规则才通过。
7. WHERE、ON、GROUP BY、写入表达式及 CHECK 禁止聚合，同层嵌套聚合也拒绝。HAVING 只能是 BOOL 或 NULL，且同样遵守分组规则。GROUP BY/HAVING 不读取输出别名；ORDER BY 沿用既有别名解析、冲突与歧义规则，并检查非分组表达式。
8. analyzeSelect 提供是否为聚合查询、投影类型与分组类型。COUNT 与整数 SUM 推断为 BIGINT，整数 AVG 为 DECIMAL(38,6)，MIN/MAX 保留参数类型；COUNT 接受任意合法标量类型，SUM/AVG 拒绝字符串和布尔参数。DECIMAL 字面量、列及组合表达式可参与五种聚合：SUM 保留输入小数位，AVG 至少六位，MIN/MAX 按数值比较。AVG 与整数可比较和执行四则运算，详见 decimal-progress.md 第六、七节及 decimal-storage-progress.md。
9. Aggregate 计划先输出去重后的分组键，再输出聚合结果槽位。groupKeys 保存源行表达式；aggregates 保存函数名、参数、结果类型、可空性和 columnId，COUNT(*) 的参数为 null。相同的聚合表达式在投影、HAVING 和排序中共享槽位，分组以上的表达式只引用结果槽位，不保留未经分组的源行引用。
10. 计划顺序保持 WHERE 在 Aggregate 下方、HAVING 在上方，之后为投影、DISTINCT、排序和分页。隐藏排序列经过同样的槽位绑定，最终输出排除隐藏列；Aggregate 及上方节点不保留原始 RowId。JSON 返回新增 groupKeys 和 aggregates 字段，HTTP compile 接口可以展示实际计划，executor 标记为 notRun。
11. 优化器可重写分组键和聚合参数中的标量表达式，仍保留 Aggregate 节点、槽位编号和输出类型。新增表达式计入统一节点预算；优化不修改调用者的原始计划，预算失败也不修改原计划。
12. 执行器对单表扫描逐行更新分组状态，JOIN 输入复用现有连接行生成器。WHERE 先过滤；分组键使用计算后的完整值元组，NULL 归入同一组。COUNT(*) 统计全部行，其他形式忽略 NULL；SUM 逐步进行 BIGINT 溢出检查，MIN/MAX 按已绑定的同类型值比较。全局空输入产生 COUNT=0、其他值=NULL 的一行；显式分组的空输入没有组。
13. 分组结果使用 JSON 计算行，保留布尔值、NULL 和整数，HAVING 与投影复用同一表达式求值器，不通过字符串化或强转 INT32 传递计算值。HAVING 后执行投影、去重、排序与分页。本节最初仅支持计算值；后续 BOOL 列持久化及 COUNT/MIN/MAX 组合见 bool-progress.md，SUM/AVG 仍拒绝 BOOL。
14. 当前最多 65536 个分组，键与状态的序列化载荷总预算为 64 MiB，超限显式报错；这一载荷预算不是完整进程内存保证，未覆盖容器开销、JOIN 输入和最终输出缓存，边界压力测试尚未完成。可配置预算、临时分区及外部聚合仍属于未完成范围，不以有限拒绝替代 V3 外存要求。

当前函数内部 DISTINCT、窗口函数与用户自定义函数不在本阶段语法范围内。它们不能因 SELECT DISTINCT 已存在而被宣称支持。

## 三、测试证据

- tests/aggregate-parser.mjs：本轮 79 项检查通过，覆盖五种函数、参数 AST、分组与 HAVING 结构、语句顺序、中文与 CRLF 位置、深度保护、错误语法、函数名作为列名以及合法查询编译；合法聚合不再要求 9001，非法 WHERE 聚合继续验 2003。
- tests/aggregate-plan.mjs：本轮 63 项检查通过，覆盖分组键和聚合槽位、跨子句复用、隐藏排序、HAVING 顺序、星号列映射、JOIN、输出类型及可空性、常量折叠、非法引用和确定性序列化。
- tests/aggregate_semantic_contract.cpp：60 项检查通过，覆盖有效分组组合、限定列、多表与星号、别名作用域、嵌套聚合、参数和结果类型、分组键整数规范化及嵌套错误位置。独立构建目标为 aggregate-semantic-test，CMake 测试目标也已登记；本轮验证使用独立构建脚本，未运行完整 CMake/CTest。
- tests/planner-regression.mjs：本轮再次通过原有 26 项语义与计划回归。
- tests/alias-process.mjs：原有 37 项别名与限定列进程检查通过。
- tests/join-process.mjs：原有 68 项 INNER JOIN 检查通过，包含 16 个 SQLite 差分查询。
- tests/parser-regression.mjs：原有 18 项词法/语法回归通过。
- tests/transaction-process.mjs：本轮再次通过原有 74 项提交、回滚、失败、未结束事务、DDL 和持久化回归。
- tests/aggregate-process.mjs：本轮 95 项检查通过，包括 23 个 SQLite 差分查询，覆盖多键与 NULL 分组、空输入、HAVING、隐藏排序、星号、布尔计算键、JOIN、BIGINT 精度与溢出、除零、事务内读到未提交写入及回滚后的重开结果。差分查询显式指定相同 NULL 排序，V3 默认升序 NULL 后置另作直接检查。
- tests/aggregate-execution-gate.mjs：本轮 25 项 AVG 保护检查通过。AVG 编译成功并包含 Aggregate，执行明确拒绝；隔离数据库数据不变，事务内先写入再执行 AVG 会回滚。
- tests/optimizer_contract.cpp：本轮 186 项检查通过，包括聚合参数折叠、原计划保护、固定点、分组键与聚合参数预算，以及聚合优化前后结果和错误等价性；既有数据库执行回归同时通过。
- tests/database-http.mjs：本轮全套通过，新增真实 HTTP 四种聚合执行及 AVG 拒绝检查，既有事务、多行 VALUES、外键及串行并发写入回归通过。
- C:/Users/王仪杰/.codex/playwright-runtime/minisql-session-ui.cjs：真实 Edge 使用隔离 C++ 数据库，四种聚合结果、GROUP BY/HAVING 查询及实际结果单元格检查通过，原有事务、标签、文件、诊断、迟到响应及 390px 布局回归通过，全程无截图。
- compile 和 database 独立入口均以 C++20、Wall/Wextra/Werror 构建通过。

以上测试记录中原先的 AVG 9001 保护已被以下新结果替代；其余历史测试不自动视作本轮重跑。

### 最新 AVG 验证

1. decimal_contract 39 项检查在 GCC 和 MSVC 均通过，最小 CMake/CTest 工程通过；完整主工程 CMake 仍未验证。
2. avg-process 58 项检查通过，包含 128 组随机 BIGINT 样本与独立精确算法对照、NULL、空输入、CAST、排序、比较及回滚。
3. aggregate-execution-gate 25 项通过，改为验证 AVG 编译无副作用、真实求值和事务回滚，不再断言合法 AVG 返回 9001。
4. 原有 aggregate-process 95 项再次通过；计划检查增加 AVG CAST 类型与共享槽位，67 项通过。
5. HTTP 全套再次通过，AVG 能力与实际响应一致。Edge 工作台真实单元格检查通过，包括 BIGINT 最大值平均值完整显示；原有事务、标签、诊断、文件与窄屏 DOM 回归通过，无截图。
6. optimizer_contract 重新构建后 194 项通过，新增 AVG、HAVING、CAST、空输入与除零参数的优化前后结果和错误等价检查。

整数 AVG 的通过不代表通用 DECIMAL、FLOAT 聚合输入或完整 X06 通过。详见 decimal-progress.md。

### 十进制算术增量回归

AVG 结果的四则运算接通后，decimal_contract 59 项、聚合语义 62 项、计划 71 项、decimal-arithmetic-process 37 项、原有 AVG 58 项及 optimizer_contract 202 项均通过。HTTP 全套与 Edge 工作台回归再次通过，新增十二位小数乘积显示检查。两套编译器验证范围分别为 GCC 核心程序与 MSVC 最小十进制测试工程，不声称主工程全量 CMake 已完成。

## 四、后续实现

1. DECIMAL 列赋值精度已实现；已完成的语义检查继续随其余类型扩展，补齐新增类型的 CAST 组合。
2. 当前执行器已消费 groupKeys/aggregates 并产生分组行，DECIMAL 列已接入，继续扩展 FLOAT 输入并验证跨模块组合。
3. 精确四则运算已接入 DECIMAL 列，继续完成其余 CAST 规则及五种聚合与新增类型的完整组合。
4. 验证 JOIN、WHERE、HAVING、DISTINCT、排序、事务快照及重启的组合，再接入内存预算与外部聚合。

EXT-SQL-006 保持部分实现，X06 未通过。当前总目标以 V3 完整详细需求规格说明书为准，包含课程核心、27 组扩展和工作台要求，不以本记录中的局部进展重新定义完成范围。
