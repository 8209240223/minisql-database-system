# CHECK 约束实施记录

日期：2026-09-08，对应 EXT-SQL-011 的部分能力。

## 一、语义规则

支持表级和列级 CHECK(expression)，表达式复用已有列引用、比较、算术、NULL、AND/OR/NOT 和 CAST。结果为 TRUE 时允许，为 FALSE 时拒绝，为 NULL/UNKNOWN 时允许；这与 WHERE 只接受 TRUE 的规则不同。创建时要求结果类型 BOOL 或 NULL，未知列和非法类型在写入前诊断。

列级 CHECK 可以重复，并可与 DEFAULT、NOT NULL、PRIMARY KEY、UNIQUE、REFERENCES 组合。项目约定列级声明与表级声明使用相同整行作用域，允许引用同表其他列，包括后面声明的列；解析完成后统一绑定。两种写法均进入 Statement.checks，不额外保留列级声明位置类别。CHECK 作为保留关键字，不能再用作未加引号的表名或列名。

INSERT 先完成整行表达式、默认值、NULL、类型和键约束检查，再执行 CHECK；UPDATE 在物理替换前检查替换行。任一 CHECK 为 FALSE 时不写入数据。

## 二、AST、计划与持久化

Statement 保存 checks 表达式数组。CREATE 计划的 checkDefinitions 保存原始 AST，保留字符串词素、CAST 目标类型和源码位置；checks 保存绑定后的执行表达式，包含 columnId、type 和 nullable。创建目录只能使用 checkDefinitions，不能把绑定后的 JSON 反向猜测为源码 AST。优化器资源计数同时覆盖两者。

CHECK 引入时的目录表描述符为 version=2，保存 name、keys 和结构化原始 checks；复合外键新增后写入 version=3，额外保存 foreignKeys。旧 version=1/2 仍可读取，version=1 没有 CHECK。此前错误版本若已经保存了语义损坏的定义，本次不会自动推断或迁移该定义，需对照原始建表语句恢复；已损坏数据的自动迁移不在本次验证范围。

CHECK 读取入口只接受 Literal、Identifier、Unary、Binary、Cast 原始节点。必填字段及类型、非负整型位置、子节点数量、操作符和 CAST 目标均校验；拒绝未知字段、NULL 节点和绑定计划字段。叶节点复用词法分析验证完整词素，不接受夹带注释或其他 SQL；标识符允许一层表限定。每个表达式最多 65536 个节点、递归深度 256、单个 value 最多 1048576 字节。词法或结构错误统一返回 Storage 错误。此接口是 CHECK 表达式协议，不代表全部 AST/Plan 的通用往返协议已经完成。

## 三、验证与边界

tests/check-process.mjs 的 68 项检查通过，覆盖 TRUE/FALSE/UNKNOWN、NULL、INSERT、UPDATE、DEFAULT、跨进程重启、计划绑定、非法表达式及失败后数据保持。新增转义字符串、CAST、嵌套条件、常量 TRUE/FALSE/NULL、列级多重约束、跨列引用和原始定义与 AST 一致性。每次 SQL 调用使用新进程，实际覆盖目录重新加载；CHECK(FALSE) 允许建表但拒绝插入，CHECK(NULL) 允许插入。

修复前，新增 CHECK(NULL) 测试得到 false 而非预期 true，确认旧版本存在回归。字符串和 CAST 通过原始定义持久化路径修复，不能以此前整数比较测试通过来证明其正确。

本轮列级 CHECK 测试先在旧程序建表时失败，再在修复后通过。heap_catalog_contract 重新构建后通过 1595 项，新增 56 项覆盖表达式往返、非法字段/节点/词素/深度、非规范操作符大小写和真实目录重新加载。损坏目录测试通过 HeapStore 写入元数据并正常 flush，保证页校验和有效，从而验证拒绝发生在 CHECK 读取层而不是页校验层。

CHECK 约束命名后续已接入，见 named-constraint-progress.md；当前表描述符写入 v4，保留 v1/v2/v3 读取。完整 AST/Plan 往返协议、多行 VALUES 整批撤销、索引加速、事务、WAL 和崩溃恢复仍待完成。外键实现详见 foreign-key-progress.md；本记录不是 X11 完整通过报告。
