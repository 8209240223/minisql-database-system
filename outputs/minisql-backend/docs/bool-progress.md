# BOOL 列与转换实施记录

对应 V3 EXT-SQL-003。BOOL 列已接通解析、目录、执行和页式存储；本记录不表示 FLOAT、VARCHAR(n)、X03 或完整 V3 已完成。后续 DATE 增量见 date-progress.md。

## 一、语言与类型契约

1. BOOL 是上下文类型名，原有名为 bool 的普通列仍可使用。TRUE/FALSE 大小写不敏感，沿用已有字面量解析，计划中为 bool 类型和真正 JSON 布尔值。
2. BOOL 列只接受 BOOL 或允许的 NULL，不隐式接受整数 0/1、字符串或 DECIMAL。DEFAULT 可为 TRUE/FALSE/NULL，NOT NULL 和主键仍拒绝 NULL；计算表达式可以产生待写入的布尔值。
3. WHERE 只接受 TRUE；FALSE 与 UNKNOWN 不选中。NULL 的布尔运算沿用 SQL 三值逻辑，短路保持从左到右，不能让优化器提前计算未执行分支里的非法转换。
4. 排序沿用既有表达式规则，FALSE 在 TRUE 前，NULL 的位置受 NULLS FIRST/LAST 控制。COUNT 忽略 NULL，MIN/MAX 保留 BOOL 类型，SUM/AVG 与算术表达式拒绝 BOOL。

## 二、显式 CAST

V3 要求各类型验证合法与非法 CAST，但没有规定 BOOL 的全部转换矩阵。本次工程约定：BOOL 可转换为 BOOL 或 VARCHAR，字符串可显式转换为 BOOL，NULL 保持 NULL；数值与 BOOL 之间的 CAST 在语义阶段拒绝。

BOOL 转 VARCHAR 输出大写 TRUE/FALSE。VARCHAR 转 BOOL 必须完整匹配 TRUE/FALSE，仅忽略 ASCII 大小写，不修剪空白，不接受 0/1、yes、空字符串或任意后缀。非法字符串在实际执行时报告 5001 并保留 CAST 位置。CHECK 反序列化允许 BOOL 目标并继续验证完整 AST，重启不跳过语义检查。

## 三、持久化与协议

1. 原有类型编号保持不变，新增 BOOL=4；Value 变体新增真正 bool 分支，不借用 INT 或字符串。JSON 响应输出 true/false/null，前端已有 Cell 布尔类型可直接显示。
2. 含 BOOL 或 DECIMAL 的行使用已有 v3 类型描述格式。BOOL 的 precision、scale、保留位都必须为零；非 NULL BOOL 载荷恰为一字节 0 或 1，其他 254 种字节值必须报告存储损坏。NULL 使用位图，不带布尔载荷。
3. 旧 INT/VARCHAR/BIGINT 的 v1/v2 编码与已有 DECIMAL v3 编码不变。旧二进制不支持 BOOL 类型，不承诺降级读取。系统目录继续保存规范化类型名，已有主键、UNIQUE、CHECK、外键及事务协议复用。
4. HTTP 增加 boolColumns、booleanEncoding=json-boolean，并在 castTargets 中列出 bool。此项只描述 BOOL 能力，不表示支持全部类型转换。

## 四、验收记录

首个测试运行在未改动核心前于 BOOL 建表失败，证明新增用例可检测原有能力缺口。以下为本次实际运行结果，历史结果不自动算作重新验证。

1. bool-column-process：94 项通过，覆盖完整九种 AND/OR 输入组合、NOT、过滤、分组、排序、COUNT/MIN/MAX、NULL、默认值、合法与非法 CAST、主键/UNIQUE/CHECK/外键、重启、事务、失败多行写入原子性、编译隔离及输出类型。
2. heap_catalog_contract：2352 项行、堆和目录检查通过，新增 TRUE/FALSE/NULL、所有 254 种非法布尔字节、逐字节截断、错误模式、混合类型及旧格式回归。
3. optimizer_contract：239 项通过，包含 BOOL 三值逻辑、短路、CAST、聚合和事务更新的优化前后结果与错误等价性。
4. decimal-journal-process bool：32 项 BOOL 提交中断及重复恢复检查通过。无参数运行的 32 项 DECIMAL 恢复和原 journal-process 的 88 项也重新通过。
5. decimal-column-process 原有 78 项再次通过，parser-regression 18 项及 planner-regression 26 项通过。database、compile、heap-test、journal-test、optimizer-test 使用 GCC C++20、Wall/Wextra/Werror 构建通过；未执行主工程完整 CMake/CTest。
6. HTTP 全套回归通过，新增 BOOL 能力声明、目录类型和布尔/NULL 响应检查。Edge 浏览器使用隔离数据库建 BOOL 表，核对真实查询结果及 true/false/NULL 单元格文本；原有事务、选区、快捷键、文件往返、独立标签、迟到响应、草稿容量错误和 390px DOM 布局回归通过，全程无截图。

恢复探针沿用相对路径，避免旧窄字符命令行对中文绝对路径的限制；真实数据库进程仍在中文工作目录下执行。BOOL 探针包含 240 条 TRUE/FALSE/NULL 混合记录，恢复时逐行核对类型和值，不只检查数量。

## 五、剩余工作

FLOAT、VARCHAR(n) 及其跨类型规则仍未完成，DATE 增量见 date-progress.md；外部聚合、索引、并发和整体 V3 验收继续实施。浏览器检查只使用 DOM 和交互断言，不生成或识别图片。
