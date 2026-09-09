# NULL、NOT NULL 与 LEFT JOIN

日期：2026-09-08。涉及 EXT-SQL-002、EXT-SQL-005、EXT-SQL-008 和 EXT-SQL-011 的相关子项，不代表全部扩展完成。

## 一、已实现语义

1. NULL 是独立缺失值，与零、空字符串、字符串 NULL 区分。支持 NULL、TRUE、FALSE 字面量和 IS NULL、IS NOT NULL。
2. 普通比较有 NULL 操作数时返回 UNKNOWN，WHERE 和 ON 仅接受 TRUE。AND、OR、NOT 按三值逻辑求值，保留左到右短路；NULL AND 错误表达式仍需计算右值，不隐藏错误。
3. 算术在操作数求值后传播 NULL，不将其作为整数处理；非布尔值仍不能作为 AND/OR 条件。TRUE/FALSE 常量可用于表达式，但尚未实现可持久化 BOOL 列类型。
4. DISTINCT 将同一位置的 NULL 作为相同去重键。按项目约定，ASC 默认 NULL 最后，DESC 默认 NULL 最前，支持每个排序键指定 NULLS FIRST/LAST。
5. 支持 LEFT JOIN 和 LEFT OUTER JOIN。ON 不匹配或为 UNKNOWN 时，为该左行补齐右侧 NULL；多个匹配仍全部保留。右侧输出列即使源表声明 NOT NULL，在外连接结果中也标记为可空。
6. CREATE TABLE 支持 NOT NULL 和显式 NULL；未指定时可空。INSERT 的已知 NULL 违规在语义阶段拒绝；UPDATE 运行时计算出的 NULL 若违反约束，在全部候选行写入前拒绝。省略 INSERT 列、多行 VALUES 和 DEFAULT 尚未启用。

## 二、持久化与兼容

非空行继续写版本 1，保持原字节结构；含 NULL 行写版本 2，在版本号和列数之后增加 NULL 位图，每列一位，空值不占普通列载荷。读取器同时支持两种版本，拒绝未知版本、截断位图及非零填充位，单行容量限制仍保留。

目录原类型字段现可保存版本化 JSON 描述，包含 version、type、nullable。原来只保存 int/varchar 的旧目录仍可读取。旧 SQL 语言没有 NOT NULL 声明能力，旧的隐式列按当前扩展的默认可空处理；早期 HTTP 曾固定报告 nullable=false，该值不是旧 SQL 实际声明的约束，本轮不把它错误恢复为 NOT NULL。

新版本可读取旧数据；旧程序不保证能读取新目录描述或版本 2 行，禁止降级后继续写同一数据文件。没有实现通用格式迁移工具、在线升级或事务化迁移。

新增关键字可能影响旧库以这些单词作为未引用表列名的 SQL；引用标识符兼容仍待实现。

## 三、验证结果

| 检查 | 实际结果 |
| --- | --- |
| null-process.mjs | 58 项通过，包含全部 AND/OR/NOT 三值组合、参考引擎对照、NULL 写入重启、NOT NULL 与 LEFT JOIN |
| heap_catalog_contract.exe | 480 项通过，包含 v1/v2 行格式、跨字节位图、损坏拒绝、旧目录读取及 NOT NULL 目录重启 |
| optimizer_contract.exe | 137 项通过，新增 NULL 化简与错误行为等价性检查 |
| join-process.mjs | 68 项通过；原 LEFT JOIN 拒绝用例移至 NULL 专项的正向语义验证 |
| database-http.mjs | 43 项通过，包含真实 NULL 更新、IS NULL 和 LEFT JOIN 补空 |
| compiler-dom.cjs | 13 项通过，实际编译 NOT NULL、LEFT JOIN 和 NULLS LAST 并展示连接计划 |
| 旧功能回归 | 别名 37 项、UPDATE 30 项、词法语法 12 项、语义计划 25 项通过 |

C++ 编译入口、数据库入口、存储和优化器契约测试已重新构建。前端构建通过，仍有既有的分块大小警告。未生成或读取图片，也未完成视觉版式或移动端验收。

跨进程持久化 7 项回归通过；原固定种子非空值语料 500 次检查通过，报告为 `tests/artifacts/fuzz-cxWLPc/report.json`。该随机运行用于已有功能兼容性，不替代 NULL 专项覆盖。

## 四、未完成范围

GROUP BY/HAVING、聚合中的 NULL、更多持久化类型、其他约束、索引 NULL 规则、事务、WAL、回滚、外存连接和完整故障注入仍未实现。当前表达式错误的写前预检查不是磁盘故障或崩溃原子性，不能自动重试不确定状态的写入。

当前行格式与低层 HeapStore 负责值编码，不单独执行 SQL 约束；NOT NULL 由语义和数据库执行入口依据 Catalog 检查。LEFT JOIN 仍物化内存中间结果，不作为大表生产连接交付。
