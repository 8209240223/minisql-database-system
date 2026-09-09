# BIGINT 数值基础进度

日期：2026-09-08。服务于 EXT-SQL-003 与聚合 EXT-SQL-006。BIGINT SQL 已接通，前四节保留此前分阶段实施记录；BIGINT 能力以第五节为准。后续现有三种类型的表达式 CAST 已接通，详见 cast-progress.md；其余类型与聚合尚未完成。

## 一、本轮实现

新增 common/arithmetic.hpp 中的 arithmetic64，覆盖有符号 64 位加减乘除。运算前检查边界，不依赖有符号整数溢出行为、不以浮点数估算边界，也不依赖非标准 128 位整数。

乘法按操作数符号分区，以除法计算安全边界；除法显式拒绝零除数以及最小负数除以负一。合法整数除法向零截断，异常保留调用方源码位置。

原 INT32 arithmetic 已复用该函数作为中间计算，再检查 INT32 结果范围。因此现有执行器和常量折叠使用了新的受检算术基础，但没有扩大既有 INT SQL 的合法范围。

## 二、测试与重放

build-core.ps1 新增 arithmetic64-test 目标，生成 bin/arithmetic64_probe.exe。CMake 同时登记探针构建目标；该探针是供差分脚本批量输入使用的程序，不应空输入执行后当作通过全部用例。

运行 tests/arithmetic64-differential.mjs：13 个边界值两两组合执行四种运算，加上固定种子 5000 对 64 位操作数的四种运算，以及未知运算符，共 20,677 次。期望值由 JavaScript BigInt 独立计算，不通过 Number 或浮点数转换。

20,677 次检查均通过，覆盖正常结果、BIGINT 溢出、除零、未知运算符及源码位置保持。重新构建的优化器 137 项、NULL/LEFT JOIN 58 项、UPDATE 30 项、HTTP 43 项回归通过。

## 三、算术阶段尚未接入（历史记录）

BIGINT 列声明、超出 INT32 的字面量推断、INT 到 BIGINT 提升、8 字节行编码、目录类型登记、CAST 及精确 JSON 输出仍待实现。当前产品仍会拒绝 BIGINT SQL，能力接口不会将其报告为已支持。

COUNT 的 BIGINT 结果、SUM 的 BIGINT 累加，以及 AVG 的 DECIMAL(38,6) 输出仍待后续接入。本轮不将聚合简化成 INT32 或浮点语义，也不修改需求目标。DECIMAL、FLOAT、BOOL 列、DATE、VARCHAR(n) 等其他类型不在本轮完成范围。

## 四、BIGINT 行存储增量

底层 ColumnType 现明确固定编号 Int=0、Varchar=1、Bigint=2，Value 增加独立 int64_t 分支。BIGINT 保存为八字节小端二进制；非空行保留 v1 头结构，可空行使用已有 v2 NULL 位图。旧 INT 和 VARCHAR 字节布局不变。行内类型由调用方 RowSchema 指定，当前类型编号还不是持久化 Catalog 中的完整类型协议。

编码器要求传入准确的值类型，不在存储层隐式截断 int64_t 到 INT，也不自动把 int32_t 提升为 BIGINT。此类转换需后续由语义与执行层决定。

heap_catalog_contract 当前 1537 项检查通过，其中 1000 项是逐行重启读取断言，不是 1000 个独立场景。新增覆盖：INT64 极值、超过 JavaScript 安全整数范围的精确往返、八字节小端顺序、载荷逐字节截断、NULL、拒绝隐式类型转换、超过两帧缓存的分页写入，以及重开后替换为 NULL/最小值再重开读取。

本轮底层写入测试直接向 HeapStore 提供 BIGINT RowSchema，尚未通过 SQL 或 Catalog 创建 BIGINT 表。SQL 语法、目录类型登记、字面量推断、算术提升、CAST 和精确 JSON 输出仍待接入，HTTP 不公布 BIGINT 可用。聚合及其他数值类型目标保持不变。

## 五、SQL、HTTP 与工作台接通

1. CREATE TABLE 支持 BIGINT 及其 NULL/NOT NULL 声明，持久化目录保存 bigint 类型；跨进程重开后恢复类型与精确值。
2. 整数字面量在 INT32 范围内推断为 INT，超出该范围但仍在 INT64 范围内推断为 BIGINT。超出 INT64 的字面量报错，不先转换为浮点数。
3. 混合 INT/BIGINT 比较与算术提升到 BIGINT；赋值允许 INT 到 BIGINT，不允许 BIGINT 隐式窄化为 INT。纯 INT 算术仍检查 INT32 溢出。
4. 执行器与常量折叠共用受检算术，保留 NULL 传播和除零错误。排序、去重、连接在内部精确整数上运行，不在十进制字符串上排序。
5. CLI 输出边界将超出 JavaScript 安全整数范围的整数编码为十进制字符串，安全范围内仍为数字；响应通过 integerEncoding 明确编码规则。columnTypes 与输出列逐项对应，空结果也保留类型。
6. HTTP 能力接口公布 bigint。React 客户端登记列类型与编码元数据；结果单元格以字符串显示，不强制 Number 转换。Demo 的 SQLite 引擎不作为上述能力的验证依据。

本轮验证：bigint-process 35 项、planner-regression 26 项、database-http 48 项、optimizer_contract 137 项通过。新增安全整数阈值两侧、空结果类型、大整数等值连接和外连接 NULL 测试。

工作台 tests/bigint-dom.cjs 使用独立临时数据库和真实 HTTP/C++ 执行，检查 INT64 最大值及其减一结果的实际 DOM 文本、列类型、编码标记和页面异常。测试通过，不生成图片，不修改用户数据库。前端 TypeScript/Vite 构建通过，仍有原有的产物大于 500 kB 提示。

限制：本节不是 EXT-SQL-003 全部完成报告；完整 CAST 矩阵、DECIMAL、FLOAT、BOOL 列、DATE、VARCHAR(n) 尚未实现（现有类型 CAST 增量见 cast-progress.md）。COUNT/SUM/AVG、GROUP BY/HAVING 同样尚未实现。大整数字符串编码不等于 AST/计划的 JSON 反序列化或往返协议已完成。
