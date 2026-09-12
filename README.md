# MiniSQL 数据库管理系统

面向“大型平台软件设计实习”的 MiniSQL 项目，包含 C++20 数据库内核、Node.js HTTP bridge 和 React 工作台。

当前 Parser 支持的完整 MiniSQL 文法、优先级、类型和语法边界见 [grammar.md](grammar.md)。

当前仓库收口的是 V3 存储、恢复、安全与结果传输阶段：

- C++ 内核：SQL 编译、逻辑计划、页式存储、目录、事务、WAL 重做恢复、B+ 树索引、约束、统计和备份恢复。
- 并发与权限：多逻辑会话、数据库级排他事务锁、会话身份、对象权限和审计；**权限判定完全消费绑定结果**，不再对 SQL 文本做任何词法扫描。
- 索引：页级 B+ 树（节点级持久化、删除借位/合并/根收缩）；唯一索引采用 **build → validate → publish** 三阶段建造；提供 `indexInspect` / `indexVerify`（堆↔索引一致性检查）/ `indexRebuild`（在线重建）。
- WAL 与恢复：**记录级日志**（扩展头带 `txId`、逻辑 LSN、`prevLsn` 链与提交/撤销记录，页记录带页级 LSN 链）、group commit、模糊检查点、日志归档与安全回收，以及双写缓冲 torn-page 防护。
- 自动检查点：按已提交写语句数、已提交 WAL 字节数、提交脏页数量、脏页比例和时间窗口触发，并记录触发原因。
- 流式传输：只读 `SELECT` 支持 NDJSON 分帧和 HTTP drain 背压；当前 C++ 执行结果仍先物化，执行器级迭代属于后续工作。
- React 工作台：真实 C++ 连接、事务、编译结果、诊断、查询历史、结果导出、存储统计和流式结果客户端。
- 双模式工作台：自研 MiniSQL 使用 8081 数据，本地演示使用 8082 独立数据；支持 Token、AST、结构化诊断、计划节点详情、SQL 格式化和结果虚拟滚动。

详细进度和未完成边界见 [V3当前实现状态.md](outputs/V3当前实现状态.md)、[MiniSQL扩展功能实施清单_V1.0.md](outputs/MiniSQL扩展功能实施清单_V1.0.md) 和 [阶段收口报告](outputs/V3阶段收口报告_2026-09-09.md)。这些文档明确区分“已实现”“部分实现”和“待完成”，不把专项测试通过等同于 X01-X27 全量验收完成。

本轮专项文档：

- [WAL 与恢复增强](outputs/minisql-backend/docs/wal-recovery-progress.md)：记录级 WAL、group commit、模糊检查点、归档/安全回收与双写缓冲。
- [绑定身份与 fail-closed 鉴权](outputs/minisql-backend/docs/x25-binding-identity-progress.md)、[访问控制进度](outputs/minisql-backend/docs/access-control-progress.md)：绑定结果驱动的对象授权与审计。
- [页级 B+ 树进度](outputs/minisql-backend/docs/page-bplus-tree-progress.md)：索引三阶段建造、一致性检查与在线重建。

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

# 一键回归（compiler | execution | storage | http | frontend | all）
cd ..
powershell -ExecutionPolicy Bypass -File .\run-minisql-tests.ps1 -Suite all
```

关键专项回归（后端目录下执行）：

```powershell
node tests\x22-fault-injection.mjs          # 提交/恢复五阶段跨进程故障注入
node tests\wal-metadata-process.mjs         # 记录级 WAL：LSN/txId/prevLsn 链与 Abort 记录
node tests\wal-group-commit.mjs             # group commit、模糊检查点、归档与安全回收
node tests\doublewrite-torn-page.mjs        # 双写缓冲 torn-page 防护
node tests\index-transaction-process.mjs    # 索引三阶段建造、一致性检查与在线重建
node tests\access-binding-process.mjs       # 绑定结果驱动的鉴权
node tests\x25-stream-http.mjs              # NDJSON 流式结果与只读保护
node tests\database-http.mjs                # HTTP 执行链路
```

工作台：

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
| `MINISQL_GROUP_COMMIT` | 设为 `1` 时合并多个提交的日志 fsync 与数据页应用（group commit），默认关闭 |
| `MINISQL_GROUP_COMMIT_BYTES` | group commit 单组最大暂存字节（默认 16 MiB），超过立即同步 |
| `MINISQL_FUZZY_CHECKPOINT` | 设为 `1` 时使用模糊检查点：记录检查点边界与截止位置并保留日志，不强制刷出缓存 |
| `MINISQL_ARCHIVE_WAL` | 设为 `1` 时检查点回收日志前先把被回收的前缀归档到 `.wal.archive.N` |
| `MINISQL_DOUBLEWRITE` | 设为 `1` 时启用双写缓冲（`.dwb`），为数据页写提供 torn-page 防护 |

WAL 采用记录级格式（扩展头记录 `txId`、逻辑 LSN、`prevLsn` 链与提交/撤销记录），
回滚写入 Abort 记录，`statistics` 的 `wal` 字段暴露 LSN 水位、扩展计数、归档与双写状态。
详见 [WAL 与恢复增强](outputs/minisql-backend/docs/wal-recovery-progress.md)。

命中统计与淘汰记录也可通过 `statistics` 命令的 `buffer` 字段读取，无需开启日志文件。

## 鉴权、会话与索引接口

授权判定只消费 C++ 绑定结果（`BindResult`），别名/派生表别名/CTE 名都是作用域名而非受权对象；
绑定不闭合时一律拒绝，不存在文本扫描兜底。一次性执行入口把 `MINISQL_USER` / `MINISQL_PASSWORD`
与所在访问目录一并交给引擎判定，bridge 与 CLI 不再自行扫描 SQL。

引擎会话协议与 HTTP bridge 暴露的维护操作：

| 操作 | HTTP 路由 | 所需权限 |
| --- | --- | --- |
| `bindAccess` | —（bridge 内部使用，CLI 也有 `bindAccess` 模式） | 身份校验 |
| `indexInspect` | `POST /api/sessions/:id/index-inspect` | 表 `READ` |
| `indexVerify` | `POST /api/sessions/:id/index-verify` | 表 `READ` |
| `indexRebuild` | `POST /api/sessions/:id/index-rebuild` | 表 `UPDATE` |
| `executePlan` | `POST /api/sessions/:id/execute-plan`（CLI 也有 `executePlan` 模式） | 按计划节点推导的对象动作 |

`executePlan` 执行 `compile` 返回的计划文档。计划在编译期绑定 Catalog 指纹（表名与列定义
的稳定哈希），执行前重新校验；不一致时拒绝执行并返回 `PLAN_STALE_SCHEMA`（HTTP 422），
不会按旧列偏移访问新数据。受权对象由计划节点推导，不重新扫描 SQL 文本。

`statistics` 的 `wal` 字段给出逻辑 LSN 水位、扩展/撤销计数、归档段与双写状态；
`checkpointRecord` 额外给出 `walLsn`、`checkpointBeginLsn` / `checkpointEndLsn` 与归档计数。

## 输入上限与错误码

第十七章列出的工程基线边界已实现，超限一律给出明确诊断而不是静默截断：

| 边界 | 默认值 | 越界表现 |
| --- | --- | --- |
| 输入 SQL | 8 MiB | HTTP 413 / 会话帧拒绝 |
| 批量语句 | 10000 | `ExecutionError`，消息含 `budget exceeded`，HTTP 413 |
| 单标识符 | 128 个字符 | `LexicalError: Identifier exceeds 128 characters` |
| 表达式嵌套 | 256 | `SyntaxError`（`2002`） |
| collectDiagnostics | 100 条错误 | 结果附 `limit` 与 `truncated` |
| Buffer Pool | 64 帧 | `MINISQL_BUFFER_FRAMES` 可覆盖 |

两个稳定符号错误码：`SEM_INTEGER_OUT_OF_RANGE`（字面量超出 INT64，或能装进 BIGINT 但装不进
INT 列时的窄化转换）与 `PLAN_STALE_SCHEMA`（计划绑定的 Catalog 指纹已失效）。

HTTP 状态映射：`400` 请求格式不合法，`403` 权限拒绝（含身份校验失败），
`413` 资源超限，`422` SQL 检查失败（含绑定失败：对象不存在等），
`501` 能力未实现（`NotImplemented`），`503` 后端不可用。

无法绑定的语句**一律不执行**（fail-closed，不存在基于 SQL 文本的兜底扫描），但错误码回报
真实原因，因此同一句 SQL 在是否配置权限目录两种情况下得到相同的状态码；身份校验在对象
授权之前完成，未授权调用方拿到的依旧是 `403`。

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

- X22 自动检查点策略扩展、后台调度与五个提交/恢复阶段跨进程故障注入；记录级 WAL 元数据、group commit、模糊检查点、日志归档/安全回收与双写缓冲均有专项回归。
- X25 传输层 NDJSON 流式结果、只读写保护、HTTP 背压和工作台客户端；鉴权链路已收敛到绑定结果，SQL 文本扫描在 C++、bridge 与 CLI 三处全部移除。
- X23 多逻辑会话并发控制、锁等待超时和关闭回滚。
- X24 访问目录（页式文件 + PersistentCatalog 系统表）、对象权限、角色继承、原子 GRANT/REVOKE 端点、身份绑定、审计过滤、直接入口身份校验和工作台权限/审计面板。
- X20 页级 B+ 树节点遍历、删除借位/合并/根收缩、结构校验、唯一索引三阶段建造、一致性检查与在线重建。
- X26 在线一致性快照、非零 WAL 截止位置重做与迁移失败回滚目录。
- 第十七章 REQ-CORE-001/002 工程基线：标识符/批量语句/诊断数量上限，`SEM_INTEGER_OUT_OF_RANGE` 与 `PLAN_STALE_SCHEMA` 稳定错误码，以及序列化计划执行入口。
- 第十九章 REQ-UI-010：`GET /api/storage/stats` 别名、未实现能力返回 501、资源超限返回 413 的完整状态映射。
- 第十七章 §7.2 极端嵌套：表达式嵌套上限 256 现在真正可用（257 起报 `SyntaxError 2002`）；派生表与标量子查询的递归补上了缺失的深度保护；嵌套标量子查询（含非相关多层）可正常执行。
- Windows 非 ASCII 路径（中文用户名等）：`snapshot` 会话操作不再把 UTF-8 字符串隐式窄转成 `std::filesystem::path`，快照响应也不再回显 `path::string()` 的 ANSI 字节，因此含中文路径的在线备份不再返回 503；`emit` 的序列化失败也不再逃逸成没有 `id` 的错误帧。
- WAL 重做校验：损坏的日志页在写入数据文件**之前**就按页自校验和被拒绝（此前会先改数据文件再以误导性的 `file header` 报错）。

全量回归当前状态：**105/105 通过**（`tests/*.mjs` 102 项 + 3 项慢用例）。

仍待继续（不把专项测试通过等同于全量验收）：

- X25 的执行器级迭代、真正提前停止与完整资源预算/压力验收。
- 执行器与 MVCC/行级隔离（当前为数据库级排他事务锁，非行锁）。
- 递归 CTE（显式返回 `NotImplemented`）与 planner 对绑定身份的进一步直接消费。
- 工作台的索引 `verify`/`rebuild` 按钮（后端接口已可用）。
- 更大规模数据下的性能/资源曲线，以及 POSIX 分支验证（当前仅在 Windows 本机验证）。
- 真实硬件断电、网络文件系统等耐久性场景验证。
- X01-X27 全量组合验收、前端最终收口和长期回归。
