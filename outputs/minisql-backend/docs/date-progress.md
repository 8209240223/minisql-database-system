# DATE 类型实施记录

对应 V3 EXT-SQL-003。DATE 已接通常量、列、显式转换、目录和页式存储；FLOAT、VARCHAR(n) 及完整 X03 仍未完成。

## 一、类型契约

DATE 使用前推公历，支持 0001-01-01 至 9999-12-31，不接受年份零、负年份或扩展年份。解析必须恰为十个 ASCII 字符 YYYY-MM-DD；日历合法性使用 C++20 std::chrono::year_month_day::ok 检查，不依赖系统区域、时区或 JavaScript Date。

DATE 是上下文类型名，不新增为全局保留字，名为 date 的普通列仍合法。DATE 后跟字符串时识别为日期常量，AST 沿用 Literal 并保存规范化的 DATE 前缀及原字符串词素，保留 DATE 起始位置。语义阶段验证常量，非法日期在不可达分支中也不能绕过静态检查。DEFAULT 接受日期常量或允许的 NULL，不接受未转换的普通字符串。

日期列只接受 DATE 或允许的 NULL；BOOL、数值与 VARCHAR 均不隐式赋值。日期比较只允许同类型或 NULL，固定 ISO 表示使时间先后与字典顺序一致。WHERE、排序、分组、去重、连接、COUNT、MIN/MAX 和约束使用已有绑定列类型；SUM/AVG、日期算术暂不属于已实现的日期操作。

## 二、CAST 与诊断

工程补充矩阵：DATE 可显式转为 DATE 或 VARCHAR，VARCHAR 可显式转为 DATE；NULL 转换仍为 NULL。数值或 BOOL 与 DATE 之间转换在语义阶段拒绝。不把日期解释为时间戳、布尔值或整数天数。

字符串 CAST 使用同一 ISO 校验器，不修剪空白，不接受时分秒、时区、短月日、尾随内容或仅合法前缀。非法日期常量报告语义错误 2003，非法 CAST 字符串在实际求值时报告执行错误 5001，保留 CAST 位置；未执行分支不提前触发运行时错误。CHECK 的序列化与恢复均允许日期常量及 DATE 目标 CAST，并验证结构和日期合法性。

## 三、存储与接口

新增稳定类型编号 DATE=5，不改变 INT=0、VARCHAR=1、BIGINT=2、DECIMAL=3、BOOL=4。含日期的行沿用 v3 类型描述与 NULL 位图，DATE 的精度、小数位和保留字节均为零。非 NULL 日期载荷固定四字节，保存相对 1970-01-01 的有符号天数，小端补码；不是字符串磁盘编码。

解码先检查天数范围，再构造日历日期，防止异常天数绕回有限年份。类型描述不匹配、截断、非法载荷与尾随字节继续拒绝。旧 v1/v2 与既有 v3 字段编码保持不变，不承诺旧二进制读取新增类型。

内存及 HTTP 使用合法 ISO 日期字符串，计划保留 date 类型。HTTP 增加 dateColumns、dateEncoding=iso-date-string，castTargets 包含 date。工作台直接显示完整字符串，包括年份前导零，不按用户本地时区偏移日期。

## 四、验证记录

新增 date-column-process 在原程序的 DATE 建表处失败，更新后首轮 115 项通过。heap_catalog_contract 的 2508 项行、堆和目录检查通过，其中完整遍历 1600-01-01 至 1999-12-31 共 146097 天，按一个完整周期用例登记，未把每日迭代次数作为独立检查数。

编码测试包含纪元前后天数、上下界、逐字节截断、非法月份/日数/闰日、超范围载荷、NULL 和模式不匹配。原先用于拒绝未知 CHECK 转换类型的 DATE 用例已改用仍未支持的 TIME，保留未知类型拒绝检查，同时通过实际 DATE CHECK 的重启用例验证新能力。

1. date-column-process 最终 122 项通过，补充两种错误的行列号、混合 DATE/BOOL/BIGINT/DECIMAL/VARCHAR 行及空输入聚合检查。
2. DATE、BOOL、DECIMAL 的日志中断测试分别 32 项通过，原 journal-process 88 项通过；恢复覆盖五个提交阶段、240 行混合日期/NULL 与连续两次重启。
3. BOOL 94 项、DECIMAL 78 项、原 CAST 81 项、词法/语法 18 项、语义/计划 26 项回归通过。
4. database、compile、heap-test、journal-test、optimizer-test 均以 GCC C++20、Wall/Wextra/Werror 构建通过。未运行本轮主工程完整 CMake/CTest 或 MSVC 全量构建。
5. 优化器首次在后续 DECIMAL 建表夹具失败，原检查只保留布尔成功断言，未捕获底层错误。现场为 tests/artifacts/optimizer-115154000680700/database.pages，原文件保留；其副本 replay.pages 的建表与插入重放成功。增加失败响应及现场路径输出后，一次重跑及连续十次重复均通过 248 项检查。首次失败根因仍未定位，不能用重跑通过宣称修复，也不能据此归因于 DATE 或环境。
6. HTTP 全套回归通过，新增 dateColumns、dateEncoding、CAST 目标、目录类型及日期/NULL 响应检查。Edge 浏览器使用隔离数据库，真实建表插入后核对 ISO 日期、前导零年份、闰日及 NULL 的响应和实际单元格文本；原有事务、标签、选区、文件、迟到响应、草稿异常及 390px DOM 布局回归通过，全程无截图。

恢复探针沿用既有相对路径约定，避免探针窄字符 argv 对中文绝对路径的限制。真实数据库入口使用中文工作目录的隔离数据库文件；不读写用户已有表。

## 五、剩余范围

日期算术、时间戳、时区、日期格式化函数未开放，不以 DATE 支持暗示这些能力。FLOAT、VARCHAR(n)、跨类型组合、索引、并发、外部聚合及 V3 全面验收继续推进。浏览器仅进行 DOM 与交互检查，不生成或识别图片。
