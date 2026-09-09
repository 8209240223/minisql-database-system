# 优化器阶段记录

日期：2026-09-08。对应 REQ-OPT、EXT-OPT-001/002 的部分能力。

## 本次实现

1. 常量比较折叠，支持当前 INT/VARCHAR 条件中的六种比较。
2. NOT 常量化简，以及 AND/OR 的左常量短路化简和右恒等值化简。保留左侧求值的必要性，不实施可能掩盖未来算术异常的右吸收值删除。
3. 恒真 Filter 消除，保留原子计划的输出及行定位。暂不删除恒假扫描，以免隐藏底层读取错误。
4. 输入计划复制后改写，保留原始计划；每条规则有独立开关及 before/after、ruleId、statementIndex 记录。
5. Database 默认执行优化后计划，可关闭优化进行真实结果对照；编译接口返回 plan、optimizedPlan、optimizationRules。独立编译器也已接入并更新阶段状态。

## 验证

tests/optimizer_contract.cpp：26 项检查通过，包括规则命中、关闭规则、原计划不变、二次优化稳定、未知列不可被恒假分支隐藏，以及 12 组优化开关前后实际查询结果一致。保留重复行、重复列、输出顺序并验证优化 DELETE。

25 项语义/计划回归重新通过；重新构建数据库命令行后，5 项独立进程持久化断言通过。新构建脚本 scripts/build-core.ps1 接受显式 JsonInclude 和目标，开启 Wall/Wextra/Werror，并已实际用于重建 compile/database。

## 初版剩余范围（历史记录）

规则目前以有界递归后序遍历应用，适用于现有三条缩减规则，不是通用规则注册与固定点框架。算术折叠、NULL、列裁剪、谓词下推、代价模型、EXPLAIN 尚未实现；不能据此把 EXT-OPT-001 或 EXT-OPT-002 标为全部完成。

原有进度文档中的“无优化器”是历史状态，本记录仅更新本次范围。HTTP/前端尚未展示优化前后结构；完整 API、交互及其测试仍须完成。当前优化器输入必须是语义检查成功的内部计划，不接受不可信外部 JSON 计划。

## 固定点框架增量（2026-09-08）

1. 当前包含常量比较、常量算术、布尔化简和恒真 Filter 消除四条规则。ruleDescriptors 返回稳定 ruleId、作用范围、前提及后置条件。
2. C++ Options 保留独立布尔开关，新增 disabledRules，按稳定 ID 禁用；未知 ID 返回 InvalidArgument，避免拼写错误被静默忽略。
3. 每轮执行完整后序改写后，使用确定性计划序列化精确比较结构。结构不变才设置 converged=true；日志增加 iteration 与 sequence。
4. maxIterations 默认 16，合法范围 1 至 64；maxNodes 默认 65536，上限 1000000。节点预算计入计划、输出列以及表达式/值 JSON 的结构和标量节点，不等于 SQL 字符数或运行时扫描行数。
5. 预算检查在复制计划之前执行，并在每轮后复核。达到轮数上限保留最后的规则改写计划，返回 OPTIMIZER_ITERATION_LIMIT，不宣称收敛；超出节点或深度预算拒绝优化。
6. 保存已出现的结构用于检测循环，检测到重复结构时返回 OPTIMIZER_CYCLE。当前内置规则均为缩减规则，测试未构造可实际触发循环的注册规则，因此该分支不能算已通过故障注入验证。
7. 表达式改写检查结果类型不变。删除 Filter 前核对表名、输出列数量/顺序/名称/类型/编号及 preservesRowId，契约不同则不删除。
8. 两个编译入口输出 optimizer 元数据，HTTP 保留该对象；工作台优化计划页显示收敛状态、轮数和诊断。

## 本轮验证

重新构建 compile、database、optimizer-test，均通过 Wall/Wextra/Werror。optimizer_contract 当前 117 项通过，新增固定点、稳定输入、轨迹顺序、轮数耗尽、命名开关、未知规则、非法预算、输入不变及 Filter 输出契约保护测试。

HTTP 35 项、UPDATE 30 项、语义计划 25 项、浏览器 DOM 11 项检查通过。浏览器实际打开优化计划页，断言显示已收敛和 2 轮。前端构建通过，仍有既有的代码分块大小警告。没有生成或读取图片。

## 固定点增量当时的剩余范围（历史记录）

本轮没有实现插件式规则注册、NULL/UNKNOWN、列裁剪、谓词下推、连接改写或代价模型。规则开关和预算参数目前是 C++ API，尚未提供 HTTP 请求参数或工作台配置控件。结构记录占用内存，目前没有独立字节预算；节点预算不替代解析器、存储或执行器资源管理。优化器输入仍必须是经过语义绑定的内部计划，不能作为外部 JSON 计划验证器。EXT-OPT-001 保持部分完成。

## NULL 常量折叠增量（2026-09-08）

1. 布尔化简现在覆盖 TRUE、FALSE、NULL 的完整 AND/OR 真值组合，以及 NOT NULL；字面量 IS NULL/IS NOT NULL 折叠为非空 BOOL。
2. 六种比较的两侧都是字面量且至少一侧为 NULL 时，常量比较规则产生 BOOL 类型的 NULL，表示 UNKNOWN。不是 FALSE；不把未知值误用于恒真 Filter 消除。
3. 新常量保留原表达式源码行列，输出列的身份、名称、类型与 nullable 契约不改写。沿用 boolean-simplification 和 constant-comparison 两个已有 ruleId，不增加规则数量。
4. NULL 与非常量表达式不能机械合并。执行器对 NULL=1/0、NULL OR CAST('bad' AS INT)=1 等仍需要求值并报告异常，优化器必须保留这些子树。禁止以右侧 FALSE/TRUE 吸收左侧可能报错表达式。
5. 新增测试先在旧优化器上失败，报 three-valued constant folds with BOOL type；实现后 optimizer_contract.exe 501 项通过。检查覆盖完整真值组合、原计划输出契约、源码位置、禁用开关、固定点、重复行、异常与位置、聚合空输入及事务内无匹配行的 UPDATE/DELETE。

恒假 Filter 尚未替换为空结果节点，因为跳过实际扫描可能隐藏页读取错误。列裁剪、冗余 Project 消除、谓词下推、索引与成本模型等仍未完成；本增量不关闭 EXT-OPT-001/002 或 V3 整体验收。之前偶发优化器夹具失败仍按 date-progress.md 记录保留，不能因本轮通过认定已修复。

### 本增量验证结果

1. optimizer-test、database、compile 均使用 GCC C++20、-Wall -Wextra -Werror 重新构建成功。未运行完整主工程 CMake/MSVC 构建。
2. 优化器 501 项、词法语法 18 项、语义计划 26 项、NULL/LEFT JOIN 58 项、CAST 81 项、聚合 95 项（含 23 条 SQLite 差分查询）、VARCHAR(n) 75 项通过。
3. HTTP 全套通过，新增断言核对 optimizedPlan 的字面量类型与值、两类 ruleId、执行结果、NULL 比较除零拒绝和全局聚合空输入。没有仅凭输出结果相同推断规则已生效。
4. 全局 Playwright minisql-session-ui.cjs 回归通过，覆盖原有选区、快捷键、UTF-8 文件、配额错误、事务、独立标签、迟到结果与 390px DOM；未生成图片。该脚本仍不是工程内可移植 CI 入口，也未新增本规则专用的 UI 断言。
5. 运行中的 8081 编译接口已验证 NULL AND FALSE、NULL OR TRUE、NULL=1 分别变成 BOOL 字面量 false、true、null；编译副本内的 optimizer_probe 未进入正式目录。4173 工作台 HTTP 200，未向用户数据库执行测试 DDL/DML。
