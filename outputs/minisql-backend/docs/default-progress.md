# DEFAULT 常量实施记录

日期：2026-09-08，对应 EXT-SQL-011 的部分能力。

## 一、语言与执行

1. CREATE TABLE 列定义支持 DEFAULT 整数、字符串或 NULL 常量，可放在 NULL/NOT NULL 声明之前或之后；重复 DEFAULT 或重复可空性声明拒绝。默认表达式、函数、子查询不在本轮支持范围。
2. 创建目录前校验默认值的词法形式、类型、INT32/INT64 范围以及 NOT NULL 约束。整数可以提升到 BIGINT，但不允许字符串隐式转数值。非法定义不能发布表。
3. INSERT 省略列时先采用已声明 DEFAULT，再采用允许的 NULL，否则拒绝。VALUES 项可以单独写 DEFAULT；无默认值的可空列得到 NULL，必填列则拒绝。DEFAULT 不能参与算术或嵌入 CAST。
4. 显式 NULL 不等同于 DEFAULT，不会被默认值覆盖。现有记录不会因为列有默认值而在查询时自动替换 NULL。
5. AST、计划和 Catalog API 暴露 defaultValue：JSON null 表示没有声明；字符串保存原始常量，例如字符串 NULL 表示 SQL DEFAULT NULL。该字段是常量源码，不是已转换后的单元格值。

## 二、持久化与兼容

目录列描述符写入 version=2，包含 type、nullable、defaultValue。保留旧纯类型字符串和 version=1 三字段描述符读取；旧版本没有默认值，不自动为其生成默认值。重新打开时重新验证默认值及类型，非法目录记录报告存储损坏。

原始字符串保存方式保留 SQL 引号转义和完整 INT64 文本，不经过 JavaScript Number。数据行页格式不变。所有目录行先编码检查再开始写入，但没有新增事务或崩溃恢复承诺。

## 三、验证与边界

default-process.mjs 的 39 项检查通过，包括跨进程重开、缺省列、显式 DEFAULT、显式 NULL、INT64 两端、引号转义、空字符串、非法声明不留下表、编译默认值计划且不持久化。

本轮 database、compile、heap-test 重新构建成功；行/堆/目录 1539 项、HTTP 62 项、语义/计划 26 项、插入列 34 项通过。前端 TypeScript/Vite 构建通过，保留原有大于 500 kB 包体积提示。既有 BIGINT 浏览器真实写入回归通过，不生成图片；DEFAULT 专项覆盖在 CLI/HTTP 层，尚无单独的默认值编辑界面。

后续已实现单列及复合 PRIMARY KEY/UNIQUE，见 unique-progress.md 与 composite-key-progress.md；多行 VALUES 的整批撤销、CHECK、外键尚未实现。此记录不是 X11 完整通过报告。

## 四、整行插入与 UPDATE DEFAULT

后续增量支持 INSERT INTO table DEFAULT VALUES，每列先填目录默认值，再填允许的 NULL，缺少必填列值时拒绝；不允许此语法附带列清单或额外 VALUES 项。AST 用 defaultValues 标记整行默认写入，计划展开为按表结构顺序排列的常量，沿用单行写入检查。

UPDATE SET column=DEFAULT 使用该列目录默认值，无默认值则按 NULL 检查；它只能作为完整赋值项，不能参与算术或 CAST。绑定计划在编译时将 Default 标记替换为对应常量。其他赋值仍基于原始行求值，不会读取同条 UPDATE 刚重置的值。

default-process.mjs 新增整行默认写入、显式 NULL 边界、UPDATE 筛选、失败后原数据保持、原行读取及 AST/计划检查。既有默认值声明和持久化测试继续保留。

本增量 database 与 compile 重新构建成功；DEFAULT 62 项、HTTP 67 项、UPDATE 30 项、语义/计划 26 项、词法/语法 12 项、插入列 34 项均通过。本次没有改动前端视图，也没有生成图片。
