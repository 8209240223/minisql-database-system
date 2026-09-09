# VARCHAR(n) 实施记录

对应 V3 EXT-SQL-003。参数化字符串已接通解析、赋值、CAST、目录与持久化；FLOAT 和完整 X03 尚未完成。

## 一、长度与类型契约

n 是最大 Unicode 码点数，不是 UTF-8 字节数、UTF-16 单元数或可见字形数。一个四字节补充平面字符算一个码点，字母与组合附加符分别计数；不执行 Unicode 规范化，不修剪空白，也不填充尾部空格。单引号转义解码后再计数，嵌入零字符按一个码点处理。

项目补充参数范围为 1 至 4294967295，使用完整无符号 32 位参数保存。零在语义阶段拒绝，负号、缺失参数、多参数和超出解析容量在语法阶段拒绝。大声明容量不代表已实现溢出页；实际记录仍受 4016 字节行容量限制，字符限制与字节限制分别检查。

普通 VARCHAR 与不同 n 的 VARCHAR(n) 属于兼容字符串类型，可以比较、赋值和构成外键；不同长度不改变比较规则。赋值检查实际内容，允许内容可容纳时由较大声明长度写入较小列，不截断。默认值在 CREATE 阶段检查；INSERT/UPDATE 超长在写页之前报 5001 并保留表达式位置，失败多行写入不能留下部分记录。

## 二、CAST 与执行

CAST(... AS VARCHAR(n)) 沿用已有数值、BOOL、DATE 和字符串到 VARCHAR 的输出格式，再检查码点上限；超长显式报错，即使调用者显式 CAST 也不截断。DECIMAL 转字符串保留其完整小数位，不先转成整数。反向的整数、DECIMAL、BOOL、DATE CAST 沿用完整字符串检查，不因为来源带长度参数而放宽校验。

计划保留 varchar(n) 类型，COUNT/MIN/MAX、GROUP BY、排序、去重、JOIN 和约束沿用既有字符串求值逻辑；MIN/MAX 保留输入长度类型，SUM/AVG 不接受字符串。CHECK 反序列化允许合法参数化 CAST，目录重启恢复 n 并重新验证约束与默认值。

## 三、行格式

新增 BoundedVarchar=6，旧类型编号 0 至 5 不变。ColumnSchema 新增独立 maxLength，不借用 DECIMAL 精度字段。含 VARCHAR(n) 的行使用 v4：版本、列数、NULL 位图、各列类型描述、各非 NULL 值。

v4 每列先写四字节既有类型描述；只有 BoundedVarchar 列紧接一个四字节无符号长度上限。其精度、小数位、保留字段均为零。字符串值仍编码为四字节 UTF-8 字节长度和完整字节，NULL 不写值但保留模式描述。解码检查声明上限一致、实际码点数、合法 UTF-8、截断和尾随数据；其他类型的载荷编码不变。

不含 BoundedVarchar 的旧行继续使用 v1/v2/v3，不批量重写已有数据。旧二进制不能读取 v4，不承诺降级兼容。单个 VARCHAR(n) 非 NULL 列的行开销为 21 字节，3995 字节数据正好满足当前行容量，3996 字节必须拒绝，即使 n 更大。

## 四、验证进度

新增进程测试先在旧核心的 VARCHAR(n) 建表处失败；更新后 75 项通过，本轮再次运行通过。2026-09-08 收尾验证结果如下，数量均指各套件检查数，不代表需求验收覆盖率：

| 测试入口 | 本轮结果 |
| --- | --- |
| varchar-column-process.mjs | 75 项通过 |
| heap_catalog_contract.exe | 2683 项通过，证据目录 tests/artifacts/heap-117075085428000 |
| optimizer_contract.exe | 重新严格构建后 257 项通过 |
| decimal-journal-process.mjs varchar/decimal/bool/date | 四种类型各 32 项通过 |
| journal-process.mjs | 88 项通过 |
| cast-process.mjs | 81 项通过 |
| date-column-process.mjs | 122 项通过 |
| bool-column-process.mjs | 94 项通过 |
| decimal-column-process.mjs | 78 项通过 |
| database-http.mjs | 全套通过，包含参数化字符串能力、真实执行、超长拒绝和目录类型 |
| 全局 Playwright minisql-session-ui.cjs | 通过，包含中文及四字节字符真实结果单元格、事务、文件、标签页与 390px DOM 检查 |

本轮 optimizer-test 使用 GCC C++20、-Wall -Wextra -Werror 重建；compile/database/heap/journal 使用此前本增量重建的二进制。未验证完整主工程 CMake/MSVC 构建。浏览器脚本仍位于用户全局 Playwright 运行目录，尚不是工程内可移植的 CI 入口。

已核实原 8081 bridge 无数据库工作进程后刷新为 PID 89528；只读能力接口返回 boundedVarchar=true、varcharLengthUnit=unicode-code-point、quarantined=false，4173 工作台返回 HTTP 200。运行日志为 tests/artifacts/live-varchar-bridge.stdout.log 与 live-varchar-bridge.stderr.log。专项 SQL 均在隔离数据库运行，未向用户数据库创建测试表。

日志恢复探针使用 240 行中文、四字节字符及 NULL；沿用既有相对路径和五个提交中断点，并逐行核对恢复值。浏览器只检查 DOM 与交互，不生成或识别图片。

## 五、剩余范围

FLOAT 及跨类型组合、溢出页、外部聚合、索引与并发仍待实现。date-progress.md 中记录的首次偶发优化器夹具失败尚未定位，后续通过不能作为其已修复的证明；完整 V3 验收仍未完成。
