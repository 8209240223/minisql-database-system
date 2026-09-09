# 真实数据库 HTTP 与编译工作台进度

日期：2026-09-08。本记录描述已经验证的实现，不替代 V3 的完整目标范围。

## 一、当前能力

1. `scripts/database-bridge.mjs` 提供真实 C++ 数据库的 catalog、compile、execute、capabilities 接口，默认绑定本机 8081。
2. SQL 经 C++ 编译器和页式执行引擎处理，数据库文件默认位于后端 `data/workbench.pages`。旧的 8080 纯编译桥接仍为独立模式。
3. HTTP 请求在单个桥接服务内排队。批次后续语句失败会返回此前成功语句及已完成数量，不宣称整批回滚；不自动重试写入。
4. 成功的 compile 返回 Token、AST、原始计划、优化计划、规则记录和阶段状态。executor 为 notRun，不将编译成功表述为执行成功。
5. 两个 C++ 入口复用 `include/minisql/sql/serialization.hpp`。保持单语句 AST 对象、多语句和空输入 AST 数组的既有契约，新增位置字段。LIMIT/OFFSET 使用十进制字符串，避免 JavaScript 大整数精度损失。
6. 工作台的 Diagnostics 页展示 Token 类型、原文、行和列；Plan 页可以切换原始与优化计划并展开规则记录。缩进使用 depth，不再把 parent 节点编号当作树深度。

## 二、验证记录

| 验证命令 | 实际结果 |
| --- | --- |
| 后端 build-core.ps1 的 database、compile 目标 | 均通过，启用 Wall、Wextra、Werror |
| node tests/database-http.mjs | 29 项断言通过，包含 12 个并发写请求串行提交 |
| node tests/parser-regression.mjs | 12 项通过 |
| node tests/planner-regression.mjs | 25 项通过 |
| node tests/database-process.mjs | 7 项跨进程持久化断言通过 |
| 前端 npm.cmd run build | 通过；仍有大于 500 kB 的代码分块警告 |
| 前端 node tests/compiler-dom.cjs | Edge 无头浏览器 9 项断言通过 |

浏览器测试实际操作连接切换、编辑器、编译、AST、Diagnostics、计划切换，并检查编译未持久化临时表及无页面脚本异常。测试只检查 DOM 和接口，不生成或读取图片；没有完成视觉版式验收或移动端验收。

## 三、运行入口

工作台：http://127.0.0.1:4173 。默认连接仍是 MiniSQL Demo；顶部连接按钮可切换到 MiniSQL C++。

真实接口：http://127.0.0.1:8081/api 。可用 `MINISQL_DB` 指定隔离数据库文件，`MINISQL_DATABASE_EXE` 指定 C++ 程序，`MINISQL_ORIGINS` 指定允许的页面来源。

## 四、未完成边界

1. 服务内排队不是数据库并发控制；多桥接实例或外部进程同时访问同一文件仍没有跨进程锁，禁止这样部署。
2. 无事务、WAL、身份权限或可靠取消。请求断开不等于写入回滚；进程异常时需要先检查状态。
3. AST 目前只有序列化，没有反序列化、完整 JSON Schema 或往返验证，不能将 EXT-CMP-003 判为完成。部分旧 AST 节点位置尚未精确绑定。
4. 编译失败仍主要返回错误对象，没有提供各阶段的部分 Token/AST 产物。执行结果没有自动附带整套编译产物。
5. 前端尚未完成各查询标签独立结果、错误定位、批次多结果选择器、连接配置和移动端布局等全部 V3 要求。
6. 其余 SQL、存储和系统扩展以总实施清单为准。本轮没有完成全部 27 组扩展。
