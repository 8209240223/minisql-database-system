# 真实数据库 HTTP 与编译工作台进度

日期：2026-09-09。本记录描述已经验证的实现，不替代 V3 的完整目标范围。

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
| 前端 npm.cmd run test:browser | Edge 无头浏览器编译工作台 + 连接/权限/设置/拖拽/移动端回归通过 |

浏览器测试实际操作连接管理、编辑器、编译、AST、Diagnostics、计划切换、权限与审计、会话状态、设置入口、侧栏边界拖动和移动端视口，并检查无页面脚本异常。测试只检查 DOM 和接口，不生成或读取图片。

## 三、运行入口

工作台：http://127.0.0.1:4173 。顶部连接管理支持保存多个 API 配置、切换用户身份、测试连接和断开当前会话；密码只用于请求，不进入连接配置 localStorage。

真实接口：http://127.0.0.1:8081/api 。可用 `MINISQL_DB` 指定隔离数据库文件，`MINISQL_DATABASE_EXE` 指定 C++ 程序，`MINISQL_ORIGINS` 指定允许的页面来源，`MINISQL_ENGINE_REQUEST_TIMEOUT_MS` 配置单次引擎请求和常驻会话请求的超时上限（默认 30000 毫秒，上限 3600000 毫秒）。

## 四、未完成边界

1. 服务内排队不是数据库并发控制；多桥接实例或外部进程同时访问同一文件仍没有跨进程锁，禁止这样部署。
2. 已接入事务、WAL、身份权限、会话取消、资源预算和备份接口；请求断开不等于写入回滚，进程异常时仍需要先检查状态。
3. AST/Plan 已有反序列化、版本包装和往返契约，但完整 JSON Schema、稳定列身份和迁移兼容仍未闭合，不能将 EXT-CMP-003 判为完成。
4. 编译失败仍主要返回错误对象，没有提供各阶段的部分 Token/AST 产物。执行结果没有自动附带整套编译产物。
5. 前端已补充连接身份、权限/审计/会话、锁等待取消、资源预算和备份入口；基础浏览器和移动端 DOM 回归已通过，活动查询取消/超时/恢复和真实备份替换恢复仍需完整 UI 场景验收。
6. 其余 SQL、存储和系统扩展以总实施清单为准。本轮没有完成全部 27 组扩展。
