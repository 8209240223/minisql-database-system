# 多行 VALUES 插入

日期：2026-09-08。对应 EXT-SQL-011 的多行写入要求，依赖单语句页批次提交。

## 一、语言与执行约定

INSERT 的 VALUES 后支持多个以逗号分隔的非空括号行，例如 `INSERT INTO t(id,n) VALUES(1,2),(3,4);`。每行必须匹配相同的目标列清单；省略列清单时按表列顺序映射。支持每个单元格使用已有的常量、算术、CAST、NULL、DEFAULT；缺省列沿用列默认值和可空规则。DEFAULT VALUES 仍是单行形式。

整个多行 INSERT 是一条语句、一个 Insert 计划、一个提交批次；affectedRows 是全部成功插入的行数。不是把多行拆成分别提交的语句。多语句脚本仍逐语句提交，后续语句失败不撤销前一条成功语句。

## 二、编译与数据一致性

Statement.valueRows 保存多行表达式，单行兼容字段 valueExpressions/values 保留原行为。多行 AST 中 valueRows 是完整行集合；首行同时存在于兼容字段，但执行器不会重复插入。LogicalPlan.insertRows 保存每行按目标 Schema 映射后的 values/expressions，单行计划继续使用原字段。优化器对所有行的表达式执行既有规则，并把 insertRows 计入节点预算。

执行器首先计算所有候选行，检查编码容量、NOT NULL、CHECK、与旧数据及批内记录的单列/复合唯一约束和外键，再写入页批次。自引用外键按旧数据加全部候选行的最终状态检查，允许前向引用和同批循环引用；不存在的父键仍拒绝。涉及实际写入或提交失败时由页批次回滚或启动重做恢复。

## 三、验证

multirow-process.mjs 111 项通过，覆盖默认值、列重排、大整数精度、表达式优化、后续行语法/类型/运行时失败、批内与已有键冲突、CHECK、NULL、超长记录、外键、自引用前向及循环引用、复合唯一键、跨页 80 行插入、多语句提交边界。失败用例逐次重开查询并比较数据库完整字节。

database_contract 49 项通过，覆盖多行发布前回滚及发布后整批恢复；optimizer_contract 167 项通过，包括多行优化开关对照；原单行 INSERT 表达式回归 45 项通过。database-http.mjs 全套通过，覆盖多行 affectedRows/语句数、失败后数据不变、AST/计划与能力报告，以及原有外键和 12 个排队并发写入。HTTP 明确区分 statementAtomicity=true 与 transactions=false。

本轮没有执行真实浏览器 DOM 回归，也没有生成截图；HTTP 测试证明工作台使用的接口契约，不替代浏览器交互验证。前一轮记录的偶发自引用外键测试失败仍未定位，不因多行专项通过而关闭。

## 四、剩余边界

尚无 INSERT SELECT、RETURNING、ON CONFLICT 或延迟约束。显式事务核心已接入，见 transaction-progress.md。写集合仍有 16384 页上限，候选行和计划有额外内存开销，没有事务溢写。已有单行/复合外键 RESTRICT 规则不因此改变。所有扩展的完整验收仍未完成，不能仅凭 X11 的多行用例通过宣布整个项目完成。
