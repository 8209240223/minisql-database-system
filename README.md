# MiniSQL 数据库管理系统

面向“大型平台软件设计实习”的 MiniSQL 项目，包含 C++20 数据库内核、Node.js HTTP bridge 和 React 工作台。

当前 Parser 支持的完整 MiniSQL 文法、优先级、类型和语法边界见 [grammar.md](grammar.md)。

当前仓库收口的是 V3 存储与结果传输阶段：

- C++ 内核：SQL 编译、逻辑计划、页式存储、目录、事务、WAL 重做恢复、B+ 树索引、约束、统计和备份恢复。
- 并发与权限：多逻辑会话、数据库级排他事务锁、会话身份、对象权限和审计。
- 自动检查点：按已提交写语句数、已提交 WAL 字节数、提交脏页数量、脏页比例和时间窗口触发，并记录触发原因。
- 流式传输：只读 `SELECT` 支持 NDJSON 分帧和 HTTP drain 背压；当前 C++ 执行结果仍先物化，执行器级迭代属于后续工作。
- React 工作台：真实 C++ 连接、事务、编译结果、诊断、查询历史、结果导出、存储统计和流式结果客户端。
- 双模式工作台：自研 MiniSQL 使用 8081 数据，本地演示使用 8082 独立数据；支持 Token、AST、结构化诊断、计划节点详情、SQL 格式化和结果虚拟滚动。

详细进度和未完成边界见 [V3当前实现状态.md](outputs/V3当前实现状态.md)、[MiniSQL扩展功能实施清单_V1.0.md](outputs/MiniSQL扩展功能实施清单_V1.0.md) 和 [阶段收口报告](outputs/V3阶段收口报告_2026-09-09.md)。这些文档明确区分“已实现”“部分实现”和“待完成”，不把专项测试通过等同于 X01-X27 全量验收完成。

## 目录结构

```text
outputs/
  minisql-backend/        C++20 数据库内核、HTTP bridge、测试和进度文档
  minisql-workbench/      React + TypeScript 工作台
  *.md                    需求、实施清单和阶段状态文档
  start-minisql-workbench.ps1
  stop-minisql-workbench.ps1
```

## 环境

- Windows x64
- Visual Studio 2022 C++ 工具链和 Windows SDK
- CMake 3.25+
- vcpkg
- Node.js 20+

后端依赖由 `outputs/minisql-backend/vcpkg.json` 固定。首次构建需要联网下载依赖。

## 构建与验证

```powershell
cd outputs\minisql-backend
. .\scripts\environment.ps1
cmake --build build\windows --config Release --parallel 4
node tests\auto-checkpoint-smoke.mjs
node tests\x22-fault-injection.mjs
node tests\x25-stream-http.mjs
node tests\database-http.mjs
```

```powershell
cd outputs\minisql-workbench
npm.cmd install
npm.cmd run build
npm.cmd run test:safety
npm.cmd run test:history
npm.cmd run test:csv
```

## 命令行用法

引擎与编译器入口都支持标准输入和 `--file/-f` 文件输入：

```powershell
# 标准输入
Get-Content .\query.sql | .\build\windows\Release\minisql_database.exe .\data\demo.pages execute

# 文件输入（两种入口同等支持）
.\build\windows\Release\minisql_database.exe .\data\demo.pages execute --file .\query.sql
.\build\windows\Release\minisql_compile.exe --file .\query.sql
```

词法/语法/语义/计划四个阶段一次输出：`minisql_compile.exe --file query.sql` 返回 `tokens`、`ast`、`plan`、`optimizedPlan` 和分阶段 `stages`。

比较运算符接受 `=`、`==`、`!=`、`<>`、`<`、`<=`、`>`、`>=`；`==` 等价 `=`，`<>` 等价 `!=`，Token 词素保留源码原文。

缓存与存储调优开关（环境变量）：

| 变量 | 作用 |
| --- | --- |
| `MINISQL_BUFFER_FRAMES` | 缓冲池帧数（默认 64），便于观察命中率与替换行为 |
| `MINISQL_BUFFER_LOG` | 页替换日志文件路径，每次淘汰追加一行（序号/策略/页号/代数/脏页/写回状态） |
| `MINISQL_MAX_RESULT_ROWS` | 单条语句结果行预算 |
| `MINISQL_TEMP_DIR` | 外部排序/聚合临时目录 |
| `MINISQL_AUTO_CHECKPOINT_*` | 自动检查点阈值（写入数、WAL 字节、脏页数/比例、时间窗口） |

命中统计与淘汰记录也可通过 `statistics` 命令的 `buffer` 字段读取，无需开启日志文件。

## 启动工作台

```powershell
cd outputs
powershell -ExecutionPolicy Bypass -File .\start-minisql-workbench.ps1
```

浏览器访问 `http://127.0.0.1:4173`。HTTP bridge 默认监听 `http://127.0.0.1:8081/api`。

启动脚本同时会启动独立数据的本地演示 bridge：`http://127.0.0.1:8082/api`。工作台中的“自研 MiniSQL”和“本地演示”分别连接这两个地址，两个数据库文件互不影响。

停止服务：

```powershell
cd outputs
powershell -ExecutionPolicy Bypass -File .\stop-minisql-workbench.ps1
```

## 当前阶段边界

已完成并验证：

- X22 自动检查点策略扩展和五个提交/恢复阶段跨进程故障注入。
- X25 传输层 NDJSON 流式结果、只读写保护、HTTP 背压和工作台客户端。
- X23 多逻辑会话并发控制、锁等待超时和关闭回滚。
- X24 后端访问目录、对象权限、角色继承、身份绑定和审计过滤。

仍待继续：

- X24 的 CLI 强制身份校验、页式 Catalog 权限存储、逐项 GRANT/REVOKE 管理和权限 UI。
- X25 的执行器级迭代、真正提前停止、完整资源预算和压力验收。
- X20 的页级 B+ 节点遍历和页内借位/合并。
- X26 的在线一致性快照、非零 WAL 重做和迁移失败回滚目录。
- X01-X27 全量组合验收、前端最终收口和长期回归。
