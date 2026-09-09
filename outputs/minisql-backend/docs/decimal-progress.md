# 精确十进制与整数 AVG

对应 V3 的 EXT-SQL-003、EXT-SQL-006。本文件记录增量实现，不表示完整 DECIMAL 类型或 X03/X06 已完成。

## 一、表示与舍入

ExactDecimal 使用 Boost.Multiprecision cpp_int 保存带符号的整数系数，并保留精度与小数位。精度范围为 1 至 38，小数位不得超过精度。构造时检查整数系数是否超过指定精度，格式化保留目标小数位，不转换为 double，不产生负零。

fromRatio 对整数分子、分母执行精确除法和 HALF_UP 舍入；负数的中点按绝对值进位后恢复符号。parse 检查完整十进制文本，不接受科学计数法、空白、NaN、尾随垃圾和隐式丢弃非零小数位；显式 rounding 参数才允许缩小小数位时舍入。文本长度上限为 1024 字节，当前用于内部平均值及显式 CAST 路径，不代替尚未实现的 SQL DECIMAL 字面量语法。

compare 先对齐小数位，再比较整数系数。因此十进制文本 2 和 10 按数值比较，不按字符串字典序比较。

## 二、AVG 执行

每组 AVG 状态保存大整数总和与非 NULL 行数。AVG 的累加不借用 SUM 的 BIGINT 累加器，因此两个 BIGINT 最大值的平均值应成功，而相同输入的 SUM 仍应按 V3 报 BIGINT 溢出。

组完成后按 DECIMAL(38,6) 进行一次 HALF_UP 舍入。没有非 NULL 输入则输出 NULL。结果在 JSON 中使用固定六位小数的字符串传输，避免 JavaScript Number 丢失精度；绑定表达式仍保留 decimal(38,6) 类型，HAVING 比较和 ORDER BY 采用数值比较。当前所有 AVG 输出同一小数位且规范化零，DISTINCT 可以正确处理这些结果。

显式 CAST 到 VARCHAR 保留规范化十进制文本；CAST 到 INT/BIGINT 先按 HALF_UP 缩小到零位小数，再使用既有整数范围检查。它不会截断非零小数位，也不会跳过目标类型范围检查。

## 三、依赖与构建

项目主 vcpkg.json 和 core-deps/vcpkg.json 共同沿用固定基线 6f1ddd6b6878e7e66fcc35c65ba1d8feec2e01f8。核心清单只声明 nlohmann-json 与 boost-multiprecision，后者的传递依赖由 vcpkg 管理，不手工裁剪头文件。当前解析到 Boost 1.85.0 和 nlohmann-json 3.11.3。

scripts/setup-core-deps.ps1 提供可复用的核心依赖安装入口，可通过 InstallRoot 指定目录；scripts/build-core.ps1 的 JsonInclude 应指向安装后的 x64-windows/include，同时含 nlohmann 与 Boost。新增 decimal-test 构建目标。主 CMake 使用 Boost::multiprecision，tests/cmake-decimal 提供只覆盖十进制值类的最小 CMake/CTest 工程。

依赖原理依据：[Boost 官方 cpp_int 文档](https://www.boost.org/doc/libs/latest/libs/multiprecision/doc/html/boost_multiprecision/tut/ints.html)。实际版本以锁定的 vcpkg 基线为准，不自动追随该网页的 latest 版本。

## 四、验证状态

1. decimal_contract：GCC C++20 严格警告构建通过，39 项精确值类检查通过；MSVC /W4 /WX 构建及最小 CMake/CTest 工程同样通过。这不是主工程全量 CMake 验证。
2. avg-process：58 项真实进程检查通过，包括两个 BIGINT 极值的平均值、SUM 独立溢出、NULL 与空输入、负数舍入、CAST、数值比较与排序、事务回滚。另用独立 JavaScript BigInt 算法核对 128 组、每组 7 个有符号 64 位样本的结果。
3. aggregate-execution-gate：25 项编译与执行隔离检查通过，AVG 可执行，编译不修改数据文件，事务回滚恢复原文件。
4. aggregate-process：95 项已有聚合回归通过，包含 23 个 SQLite 差分查询；该套件不以 SQLite 浮点 AVG 作为精确十进制参考。
5. database-http：全套回归通过，验证 AVG 能力声明、真实平均值响应及既有事务、外键、串行写入。
6. 聚合语义 60 项、计划 67 项已通过；optimizer_contract 重新构建后 194 项通过，新增 AVG、HAVING、CAST、空输入和除零参数的优化前后结果及错误等价检查。
7. Edge 浏览器真实隔离数据库回归通过，AVG 普通结果和 BIGINT 最大值平均值均验证实际单元格文本。原有事务、标签、诊断、文件导入导出、迟到响应及 390px 布局 DOM 检查通过，全程无截图。脚本位于用户全局 playwright-runtime/minisql-session-ui.cjs，尚未打包为项目内可移植 QA 入口。

## 五、仍需完成

1. DECIMAL(p,s) 列定义和磁盘格式已接通，见 decimal-storage-progress.md；继续扩展其余类型及组合验收。
2. 与其余未实现类型的 CAST 组合；INT/BIGINT/VARCHAR/DECIMAL 间显式转换、持久化 DECIMAL 赋值精度检查及 AVG 四则运算已接通。
3. FLOAT 输入上的聚合及与其余扩展的组合；DECIMAL 表达式与持久化列均已接通五种聚合。
4. 可配置执行资源预算、外部聚合、完整 X03 与 X06 验收。

## 六、十进制表达式四则运算

decimal_type.hpp 集中解析内部 DECIMAL(p,s) 类型及推断算术小数位，语义和计划层共同调用，不各自硬编码 decimal(38,6)。整数参与十进制运算时小数位为零。加减取较大小数位，乘法为小数位之和，除法取较大小数位且至少六位；一元正负号保留类型。推断小数位超过 38 时报告语义错误，不自动削减小数位。

V3 未给出四则运算完整的结果总精度公式，本实现采用 DECIMAL(38,s) 作为表达式结果容量，并在运行时检查实际系数是否超过 38 位，不把声明容量的简单相加作为拒绝依据。这样 AVG 的 DECIMAL(38,6) 仍能参与加减，而真正的进位或乘法溢出会明确失败。该工程选择不改变 V3 的小数位规则，不隐式丢弃精度。

ExactDecimal::arithmetic 用大整数对齐小数位、乘系数或进行有理数除法；仅除法最终一步按 HALF_UP 舍入，过程中不经浮点。数值溢出和除零携带运算符源码位置。执行层的比较、排序、一元运算与 CAST 读取实际类型的小数位，避免十二位乘积被当成六位平均值重新解析。

本节实施时真实 SQL 输入来自 AVG 及其组合表达式；第七节进一步接通小数字面量，后续列持久化见 decimal-storage-progress.md。NULL 与短路沿用既有规则，不能为折叠未执行分支而提前触发算术错误。

本轮验证：十进制值类扩展至 59 项，GCC 与 MSVC/最小 CTest 均通过；聚合语义 62 项通过；decimal-arithmetic-process 的 37 项真实 SQL 检查通过，包括混合小数位、结果类型、HAVING、排序、CAST、NULL、溢出、错误位置和回滚。compile/database 独立入口均严格警告构建通过，聚合计划 71 项、优化器 202 项及原有 AVG 58 项回归通过。HTTP 全套通过，新增乘积与商的精确文本断言；Edge 工作台实际单元格保留十二位小数，既有事务、标签、诊断、文件和窄屏 DOM 回归通过，无截图。

## 七、小数字面量与十进制聚合

词法接受小数点两侧都有数字的十进制形式，例如 3.14；正负号由 Parser 组合。小数作为完整 DECIMAL Token 输出，保留原词素和起始位置，不拆成多个整数。尾随点、多重点、紧邻标识符及指数形式明确拒绝；.5 不在当前语法内，FLOAT 指数形式仍待实现。

字面量的小数位按原始小数部分长度确定，包括尾零。精度为去掉整数前导零后的整数位数加小数位数；纯小数的精度等于小数位。两者不得超过 38。结果文本去掉冗余整数前导零、保留尾零并规范化负零；超过 38 位在语义阶段报告，不做浮点解析。

优化器对十进制常量四则运算复用 ExactDecimal；比较先按数值对齐小数位，不使用 JSON 字符串字典序。常量折叠遇到运行时溢出或除零保留原子树，继续遵守短路语义。编译入口因此也依赖 Boost 头文件，构建脚本已补依赖检查。

十进制 SUM 输出 DECIMAL(38,s)，s 为参数小数位，累加检查溢出。AVG 输出 DECIMAL(38,max(s,6))，按整数系数累计后用行数和输入小数位还原分母，最后一次舍入；其总和可超过 38 位，但最终结果必须可表示。MIN/MAX 使用数值比较，COUNT 沿用非 NULL 计数。空输入保持既有聚合规则。

含十进制比较的 CHECK 可以持久化并在重启时重新验证；本节最初没有开放 DECIMAL 列，后续已新增 DECIMAL 类型编号及 v3 行编码，见 decimal-storage-progress.md。小数隐式写入 INT/BIGINT 仍拒绝，显式 CAST 后可以写入。HTTP 单独暴露 decimalExpressions、decimalColumns 和 decimalEncoding，不能用表达式支持冒充列存储支持。

本轮 compile/database 独立入口均以 GCC C++20 严格警告构建通过。decimal-literal-process 53 项通过，覆盖真实 SQL、精度、聚合、词法拒绝、CHECK 重启、AVG 溢出位置和赋值限制；原有 AVG 58 项、词法/语法 18 项通过。optimizer_contract 重新构建后 212 项通过，新增十进制常量算术、数值比较、短路、溢出及聚合的优化前后等价检查。HTTP 全套和 Edge 工作台回归通过，真实单元格核对 0.3 和 1.100000，未截图。本轮未重新运行 MSVC/全量 CMake，不把前两轮的局部构建结果当作本轮全量验证。
