# MiniSQL

面向大型平台软件设计实习的 C++20 数据库项目，使用 **VS Code** 开发。总体目标包括 SQL 编译、页式存储、查询执行、索引、事务恢复和 Web 可视化。

## 当前进度

已实现工程基础：CMake 构建、vcpkg 依赖管理、spdlog 日志、统一异常与错误码、Status/Result、JSON 配置、CLI11 命令行和 GoogleTest/CTest 测试。已提供 VS Code 构建任务与断点调试配置。

独立核心程序 `bin/minisql_database.exe` 已接入 SQL 编译、页式存储、目录持久化、查询与写入执行，支持多行 VALUES、显式事务及重做恢复；session 模式和 HTTP 会话路由支持跨请求事务，SessionRegistry 管理多个逻辑会话，并通过数据库级排他两阶段锁串行化事务写入。Node HTTP 桥位于 `scripts/database-bridge.mjs`，提供真实执行、健康检查 `/api/health`、文件页统计 `/api/storage`、能力发现 `/api/capabilities` 和只读 NDJSON 流式执行 `/api/execute/stream`。React 工作台已接入事务工具栏、查询历史、主题、结果上限、CSV 导出与复制选区。具体能力、部分实现边界和未完成项见 docs 及上级扩展实施清单。

原工程壳的 `--execute` 和 `--mode server` 仍返回 `NotImplementedError`，不要与独立核心入口混用。实际核心构建入口是 `scripts/build-core.ps1`，执行参数为数据库文件路径和 execute/compile/catalog 模式，SQL 从标准输入读取。

## 在 VS Code 中开始

1. 用 VS Code 打开本项目根目录。
2. 安装工作区推荐的 **C/C++** 和 **CMake Tools** 扩展。
3. 按 **Ctrl+Shift+B** 执行默认 Debug 构建任务。
4. 在“终端 → 运行任务”中选择 **MiniSQL: test Debug** 运行测试。
5. 按 **F5**，选择 **MiniSQL: CLI (MSVC)**，启动交互命令行并调试。也可选择 GoogleTest 调试配置。

构建任务通过 PowerShell 自动寻找 MSVC 配套的 CMake 和 vcpkg。VS Code 是编辑和调试入口，MSVC 是底层编译器，无需在 Visual Studio IDE 中操作。

### 环境要求

- Windows x64；
- VS Code 和上述两个扩展；
- Visual Studio 2022 Build Tools 或现有 Visual Studio 的 C++ 桌面开发工具，包括 MSVC v143 和 Windows SDK；
- CMake 3.25+；脚本可查找 VS 安装附带的版本；
- Git 和 vcpkg；脚本可查找 VS 安装附带的 vcpkg；
- 首次依赖安装需要联网。

本机已经具备 MSVC、Windows SDK、CMake、Git 和 vcpkg。若使用独立 vcpkg，请在终端设置 `$env:VCPKG_ROOT` 为其安装目录。

React/TypeScript 工作台已位于 `../minisql-workbench`，需要 Node.js 20+。在工作台目录执行 `npm.cmd install` 后运行 `npm.cmd run dev`，默认地址为 `http://127.0.0.1:4173`。真实 C++ 数据库 HTTP bridge 在本目录执行 `node scripts/database-bridge.mjs`，默认 API 地址为 `http://127.0.0.1:8081/api`；工作台连接 MiniSQL C++ 后即可使用真实编译、执行、事务和存储统计。

也可以在上级 `outputs` 目录执行 `powershell -ExecutionPolicy Bypass -File ./start-minisql-workbench.ps1` 一键启动 bridge 和工作台。
执行 `powershell -ExecutionPolicy Bypass -File ./stop-minisql-workbench.ps1` 可停止 8081 和 4173 的 Node 服务。
bridge 提供 `GET /api/health` 健康检查；`status=ok` 表示引擎未进入故障隔离状态。

## 构建与测试

在 VS Code 的 PowerShell 终端、项目根目录执行：

```powershell
# Debug 构建（第一次会自动安装依赖）
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/bootstrap-tools.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/build.ps1

# 构建并执行 GoogleTest + 进程级 CLI 测试
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/test.ps1

# Release 构建与测试
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/test.ps1 -Configuration Release
```

`ExecutionPolicy Bypass` 只作用于该脚本进程，不修改系统执行策略。依赖版本由 `vcpkg.json` 中的 baseline 固定；不需要全局安装各个 C++ 库。

如需直接使用 CMake 命令，先在同一终端加载工程环境：

```powershell
. ./scripts/environment.ps1
cmake --preset windows
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Debug 可执行文件位于 `build/windows/Debug/minisql.exe`；测试程序位于 `build/windows/tests/Debug/minisql_tests.exe`。Release 使用对应的 `Release` 子目录。vcpkg 动态库会随 CMake 构建复制到可执行文件所在目录，运行时请保留这些 DLL。

Linux 提供 `linux-debug` configure/build/test preset，需自行安装 C++20 编译器、Ninja 和 vcpkg，并设置 `VCPKG_ROOT`。Linux 路径未在当前 Windows 环境中实测。

## 运行

```powershell
# 查看帮助与版本
./build/windows/Debug/minisql.exe --help
./build/windows/Debug/minisql.exe --version

# 只检查配置，不创建运行目录和日志
./build/windows/Debug/minisql.exe --config config/default.json --check-config

# 输出最终生效的配置 JSON，验证端口和日志级别覆盖
./build/windows/Debug/minisql.exe --config config/development.json --port 9090 --log-level WARN --print-config

# 启动交互命令行
./build/windows/Debug/minisql.exe --config config/development.json
```

交互命令：

| 命令 | 功能 |
|---|---|
| `.help` | 显示支持的命令 |
| `.version` | 显示版本 |
| `.config` | 显示生效配置 |
| `.quit` / `.exit` | 正常退出 |

读取到 EOF 也会正常退出并刷新日志。输入 SQL 时返回结构化的未实现错误，交互会话继续可用。

## 配置

覆盖顺序是 **命令行 > 环境变量 > 指定 JSON 文件 > 内置默认值**。

- 默认不隐式读取磁盘配置；用 `--config` 指定文件，或设置 `MINISQL_CONFIG`。
- 指定的配置文件必须存在且合法，不能静默退回默认配置。
- 配置文件可以只包含需要覆盖的字段；未知字段、错误类型、null 和非法数值会报错。
- 所有相对运行路径均相对于**进程工作目录**，不是配置文件所在目录。
- `--data` 仅覆盖数据目录，不自动改变 WAL/Catalog 目录；需要时同时传入 `--wal-dir` 和 `--catalog-dir`。
- `--print-config` 同时校验配置，输出纯 JSON；与 `--check-config` 同时出现时优先输出 JSON。

```powershell
$env:MINISQL_PORT = '9090'
./build/windows/Debug/minisql.exe --print-config
# 此处 CLI 的 9091 覆盖环境变量中的 9090
./build/windows/Debug/minisql.exe --port 9091 --print-config
Remove-Item Env:MINISQL_PORT
```

默认配置见 [config/default.json](config/default.json)，完整字段、环境变量及错误规范见 [基础框架说明](docs/foundation.md)。

## 日志与错误处理

日志按 common、server、sql、catalog、execution、optimizer、storage、transaction、recovery 分文件记录。支持等级过滤、线程安全写入、文件轮转、请求 ID 和事务 ID。控制台日志输出到 stderr，配置 JSON 输出到 stdout。

默认日志位置为 `./logs`，单文件达到 5 MiB 后轮转，保留 3 个历史文件。正常退出时刷新；这是诊断日志，不是数据库 WAL，也不承诺断电持久性。

日志调用方不能传入密码、Token 或原始 SQL 等敏感数据；目前 CLI 只记录 SQL 被拒绝的事件，不记录 SQL 正文。控制字符会被转义，避免一条消息伪造多行记录。

业务错误统一为 `MiniSqlError`，携带错误码、类型、消息、位置和建议；可序列化为 JSON。模块返回值也可使用 `Status` 和 `Result<T>`。CLI11 参数格式错误使用帮助式文本；业务/配置错误使用 JSON。成功、帮助和版本退出码为 0，业务错误为 1，参数解析错误使用 CLI11 的非零退出码。

## 目录结构

```text
.
├── .vscode/                VS Code 扩展、任务和调试配置
├── CMakeLists.txt          构建目标和依赖
├── CMakePresets.json       Windows/Linux 构建与测试预设
├── vcpkg.json              依赖清单和版本 baseline
├── cmake/                  版本头文件模板和 Windows triplet
├── config/                 默认、开发、测试配置
├── include/minisql/        公共头文件
├── src/
│   ├── common/             错误、配置、日志
│   ├── server/             参数解析与 CLI 应用入口
│   ├── sql/                SQL 编译（待实现）
│   ├── catalog/            元数据（待实现）
│   ├── storage/            页与缓冲池（待实现）
│   ├── execution/          执行器（待实现）
│   ├── optimizer/          优化器（待实现）
│   ├── transaction/        事务恢复（待实现）
│   └── main.cpp
├── tests/                  单元、集成及后续专项测试目录
├── ui/                     可视化界面位置（待实现）
├── scripts/                环境发现、构建和测试脚本
└── docs/                   基础框架开发说明
```

实际静态库目标为 `minisql_common` 和 `minisql_server`，程序目标为 `minisql`，测试目标为 `minisql_tests`。其他模块当前为 CMake INTERFACE 目标，尚无实现文件。

`build/`、`.tools/`、`data/` 和 `logs/` 为本地生成目录，已加入忽略规则。部分 VS 内置旧版 vcpkg 仍会使用用户目录下的 registry 缓存，脚本不修改其安装文件。

## 后续开发

建议下一步实现 Lexer、Token 和 SQL 源码位置，再实现 Parser/AST、Catalog、存储页和基础执行闭环。索引、事务恢复、查询优化、服务 API 和可视化工作台随后接入。

公共基础库不依赖网络、UI 或 SQL 模块。新增模块时保持单向依赖，在 `tests/unit` 或 `tests/integration` 添加有意义的用例，更新 CMake 源文件清单。

## 相关文档

- [基础框架说明](docs/foundation.md)
- [技术方案与项目骨架](技术方案与项目骨架.md)
- [需求分析文档](需求分析文档_SQL编译器与MiniSQL系统_完整范围修订版.docx)
- [详细设计文档](详细设计文档_SQL编译器与MiniSQL系统.docx)
