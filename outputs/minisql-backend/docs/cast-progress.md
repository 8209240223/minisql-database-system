# CAST 显式转换实施记录

日期：2026-09-08。对应 EXT-SQL-003 的部分能力，不代表所有类型扩展完成。

## 一、转换契约

1. 表达式支持 CAST(expression AS INT/BIGINT/VARCHAR/DECIMAL(p,s))，大小写不敏感，支持嵌套与现有 256 层深度限制。可用于 SELECT、WHERE、ORDER BY、JOIN ON 和 UPDATE SET；目标 DECIMAL 的增量边界见第五节。
2. INT 与 BIGINT 互转时检查目标范围。数值转 VARCHAR 输出精确十进制字符串，不经过浮点数；VARCHAR 转整数要求完整字符串合法，接受一个可选正负号、前导零，拒绝空串、空白、双重符号、小数、指数、后缀与越界。
3. NULL 转换后仍为 NULL，计划记录目标类型与可空性。此处最初未纳入 BOOL；后续 BOOL 与 VARCHAR 的显式转换及拒绝数值转换的规则见 bool-progress.md，不隐式改变既有字符串与整数比较规则。
4. AST 使用 Cast 节点，value 为目标类型，left 为源表达式。绑定计划使用规范化 type，并保留源码位置。
5. 非法目标或源类型在编译阶段诊断；内容非法或范围越界在实际执行转换时报告 5001 并定位 CAST。优化器可以优化其子表达式，但不提前执行转换，因此不会让被短路或 LIMIT 0 跳过的转换提前报错。

## 二、验证

tests/cast-process.mjs 包含 81 项断言，覆盖三种现有类型、INT32/INT64 边界、NULL、全串解析、错误位置、类型诊断、嵌套深度、短路、AST、UPDATE 及跨进程持久化。专项检查通过。

tests/optimizer_contract.cpp 增加九组开启/关闭优化的结果及错误一致性检查；tests/database-http.mjs 增加 HTTP 类型、精确文本及窄化失败检查。各项运行结果以实际测试输出为准。

本轮最终结果：重建 database、compile、optimizer-test 成功；优化器 146 项、HTTP 51 项、BIGINT 35 项、语义/计划 26 项、词法/语法 12 项及现有浏览器 13 项均通过。首次 HTTP 运行在构建及其他测试并行期间超时，待任务完成后独立重跑通过，未修改测试超时阈值；尚不能据此证明高负载下无超时风险。浏览器回归为既有编译链路检查，CAST 专项验证在 C++/HTTP 层进行，未生成图片。

## 三、未完成项

INSERT VALUES 已在后续增量接通表达式，详见第四节；省略列清单与缺省可空列已接通，见 insert-columns-progress.md。DEFAULT 常量见 default-progress.md，多行插入见 multirow-progress.md。DECIMAL 表达式、显式转换及列持久化已接通，列赋值规则见 decimal-storage-progress.md；BOOL 列与字符串转换见 bool-progress.md，DATE 与严格 ISO 字符串转换见 date-progress.md。FLOAT、VARCHAR(n) 及其转换仍待实现，不把当前子集当作 X03 全部通过。

## 四、INSERT VALUES 表达式

1. INSERT 每个 VALUES 项保存为 valueExpressions AST，允许 CAST、算术和现有常量表达式。全部为字面量时仍保留旧 values 文本字段，兼容现有消费者；混合表达式时必须读取新字段，不把不完整的旧字段当成全部输入。
2. 类型检查和绑定使用空列作用域，禁止引用目标表列或同条插入的其他值。目标列映射仍按显式列清单建立，表达式最终按表结构顺序排列。
3. 计划新增 insertExpressions 并纳入序列化、优化遍历和资源预算检查。非空时它是执行输入；values 仅作为字面量兼容视图，不用于提前执行 CAST 或算术。
4. 执行器先计算并检查整行，再调用 HeapStore 插入。因此转换失败、除零、溢出和 NOT NULL 失败不会新增本条记录；这不代表事务或磁盘故障原子性已实现。
5. tests/insert-expression-process.mjs 的 45 项检查通过，覆盖显式转换、算术提升、列映射、类型及作用域拒绝、失败后行数不变、编译无副作用、跨进程持久化和批处理中前条成功结果保留。

本增量重建 database、compile、optimizer-test 后，词法/语法 12 项、语义/计划 26 项、优化器 151 项、HTTP 55 项、CAST 81 项及独立进程持久化 7 项通过。工作台 bigint-dom.cjs 改为真实执行 INSERT CAST，并验证 INT64 最大值与其减一结果精确显示。首次浏览器测试发现关闭时未完成的目录请求导致清理异常，修复为等待路由处理完成后关闭，重跑正常退出。测试使用独立数据库，不写用户数据，不生成图片。

## 五、DECIMAL 目标类型

Parser 要求 DECIMAL(p,s) 提供两个无符号整数参数，并规范化大小写和参数前导零。参数语法非法返回语法错误，p 不在 1 至 38 或 s 不在 0 至 p 时由语义阶段拒绝，即使源值为 NULL 也不绕过类型校验。DECIMAL 暂采用上下文类型名，未新增为全局保留字，不改变旧表的同名列。

INT、BIGINT、VARCHAR 和 DECIMAL 可显式转为 DECIMAL(p,s)。VARCHAR 检查完整十进制文本，不接受空白、后缀、指数、双重符号或非有限值。缩小小数位按 HALF_UP 舍入，随后检查精度；进位导致超出目标精度也报错，不截断。扩大小数位补零，目标 s=0 输出无小数点，负零规范化。

执行器先处理 DECIMAL 目标，再处理既有 DECIMAL 转整数路径，避免嵌套转换被提前舍入到整数。NULL 保持 NULL，错误保留 CAST 起始位置。优化器仍只优化子表达式，不提前执行 CAST；短路、空输入等执行规则保持一致。

CHECK 的反序列化器允许合法的参数化 DECIMAL 目标，仍拒绝非法类型和 AST 结构。DECIMAL 值隐式写入 INT/BIGINT 仍拒绝，必须再显式转为目标整数类型。本节最初仅实现转换；后续已开放 DECIMAL 列定义及 v3 行格式，见 decimal-storage-progress.md。

本轮 decimal-cast-process 62 项、原有 cast-process 81 项、CHECK 68 项及聚合计划 75 项通过；compile/database 独立入口以 GCC C++20 严格警告构建成功。optimizer_contract 重新构建后 220 项通过，新增参数化 CAST、舍入、错误、短路、聚合及规范化分组键等价检查。HTTP 全套及 Edge 工作台回归通过，真实单元格核对平均值缩小小数位后的 1.13，原有事务、编辑、文件、迟到响应及 390px 布局 DOM 检查通过，无截图。本轮未运行 MSVC 或全量 CMake。前述第二、四节为历史阶段测试，不自动当作本轮重新运行的结果。
