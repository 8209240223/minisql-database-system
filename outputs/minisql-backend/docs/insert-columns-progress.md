# 插入列清单与缺省 NULL

日期：2026-09-08。对应 EXT-SQL-011 的部分能力。

## 一、已实现契约

1. INSERT INTO table VALUES(...) 省略列清单时，按持久化 Catalog 中的列顺序映射；值数量必须等于表列数，不能把尾部少值默认为缺省列。
2. 显式列清单允许只提供部分列。缺省可空列填 NULL，不填 0 或空字符串；缺省 NOT NULL 列在语义阶段拒绝。
3. 列名大小写不敏感，重排列清单仍正确映射；重复、未知列和空括号列清单均拒绝。值支持已有 CAST 与算术表达式。
4. 语义检查与计划生成共用 insertColumns，避免各自推测 Schema 顺序。表达式计划为缺省列创建 NULL 字面量节点；执行沿用完整行检查后再写入的路径。

## 二、测试范围

tests/insert-columns-process.mjs 的 34 项断言通过：省略清单、部分列、大小写、重排、缺省 INT/BIGINT/VARCHAR 的 NULL 值、值数量不匹配、NOT NULL、未知与重复列、编译无数据副作用、跨进程重开，以及多行 VALUES 仍拒绝且零新增记录。

旧 planner-regression 的缺列测试原本验证“所有缺列拒绝”，现在按需求改为“缺失 NOT NULL 列拒绝”，可空缺列的成功路径在新增专项测试中覆盖。

本轮重建 database 与 compile 成功；语义/计划 26 项、HTTP 59 项、INSERT 表达式 45 项、NULL/LEFT JOIN 58 项、词法/语法 12 项均通过。既有 bigint-dom 浏览器测试也正常退出，证明真实 HTTP/C++ 插入与大整数结果显示没有回退；新增缺省列行为的专项证据来自 CLI/HTTP 测试。浏览器测试使用隔离数据库且不生成图片。

## 三、未完成边界

后续已接通 DEFAULT 常量元数据，缺省列优先采用默认值、再采用允许的 NULL，详见 default-progress.md。单列及复合 PRIMARY KEY/UNIQUE 见 unique-progress.md 和 composite-key-progress.md；CHECK、外键及其索引一致性仍待实现。

多行 VALUES 仍不启用。需求明确要求单条语句任一行失败时整批撤销，必须先满足事务/撤销依赖；不能把逐行提交的循环称作完整多行插入。这里的错误前零写入，也不代表磁盘故障或崩溃恢复原子性已实现。
