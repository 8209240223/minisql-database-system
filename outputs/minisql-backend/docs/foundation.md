# 基础框架开发说明

## 实现范围

本阶段实现 CMake、vcpkg、日志、异常、配置、命令行参数和 GoogleTest 基础设施。SQL、存储、优化、事务、网络和可视化尚未实现。代码入口为 `src/main.cpp`，应用逻辑位于 `src/server/application.cpp`。

VS Code 使用 `.vscode/tasks.json` 调用同一套 PowerShell 构建脚本，F5 调试通过 `cppvsdbg` 使用 MSVC 生成的 PDB。不需要启动 Visual Studio IDE。

## 构建依赖

`vcpkg.json` 使用 manifest 模式并锁定 baseline，主要依赖版本如下：

| 依赖 | 版本 | 用途 |
|---|---|---|
| CLI11 | 2.4.2 | 参数解析、帮助、版本 |
| fmt | 10.2.1 | 类型安全格式化 |
| spdlog | 1.14.1 | 分模块线程安全日志和文件轮转 |
| nlohmann-json | 3.11.3 | 配置和诊断序列化 |
| GoogleTest | 1.14.0 | 单元/集成测试 |

Windows 的 spdlog 启用 `wchar` feature，允许日志路径包含中文或其他 Unicode 字符。GoogleTest 是 manifest 的 `tests` feature；`BUILD_TESTING=ON` 时在 CMake `project()` 之前启用它。

依赖关系：

```text
minisql → minisql_server → minisql_common
                      ↘ CLI11
minisql_common → fmt / spdlog / nlohmann_json / minisql_options
minisql_tests → minisql_server / GTest::gtest_main / Threads
```

其余模块目前仅有 CMake INTERFACE 目标，可在实现时改为 STATIC 目标并显式列出源文件。

vcpkg manifest 与 preset 用法参考 [Microsoft manifest 文档](https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode)；CLI11 API 参考 [项目官方文档](https://github.com/CLIUtils/CLI11)。

## Windows 构建环境

`scripts/environment.ps1` 查找 PATH 或 MSVC 配套的 CMake，然后选择显式 `VCPKG_ROOT` 或 VS 配套 vcpkg。所有环境变量只修改当前脚本进程，不永久修改 PATH。构建复用现有 CMake；本项目所锁定的库支持 CMake 3.25+。

旧版 VS 内置 vcpkg 可能引用已经移除的 MSYS2 pkgconf 下载文件，出现 HTTP 404。为适配该环境，先将官方 pkgconf 3.0.7 工具解包到项目目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/bootstrap-tools.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File ./scripts/build.ps1
```

脚本从 [pkgconf 官方发布](https://github.com/pkgconf/pkgconf/releases/tag/pkgconf-3.0.7) 下载 x64 MSI，验证固定 SHA-256 后以 administrative extraction 方式解包到 `.tools/pkgconf`，不会执行产品的系统安装。Windows triplet 检测到此工具后通过 `PKG_CONFIG` 提供给 vcpkg；用户显式设置的 `PKG_CONFIG` 优先。

`cmake/triplets/x64-windows.cmake` 在依赖构建的 CMake 子进程内将 TEMP/TMP 设置为 `.tools/tmp`，规避旧版 vcpkg 捕获开发环境时中文临时目录失真的问题。标准 x64 Windows 动态库与动态 CRT 设置保持一致。

## 配置字段

| JSON 字段 | 默认值 | 校验 |
|---|---|---|
| data_directory | ./data | 非空、不含 NUL |
| wal_directory | ./data/wal | 非空、不含 NUL |
| catalog_directory | ./data/system | 非空、不含 NUL |
| page_size | 4096 | 512～65536 的 2 次幂，单位字节 |
| buffer_pool_size | 128 | 1～1048576，单位页 |
| replacement_policy | LRU | LRU/FIFO，大小写归一化 |
| mode | cli | cli/server |
| server.host | 127.0.0.1 | 非空、不含 NUL；本阶段不解析或绑定地址 |
| server.port | 8080 | 1～65535 的整数 |
| server.websocket_enabled | true | JSON 布尔值 |
| logging.level | INFO | TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL/OFF，大小写归一化 |
| logging.directory | ./logs | 非空、不含 NUL |
| logging.console | true | JSON 布尔值 |
| logging.max_file_size | 5242880 | 1024～1073741824 字节 |
| logging.max_files | 3 | 1～100 个轮转历史文件 |

缓存容量只作为配置项保存，不会在本阶段分配数据库缓冲页。`page_size` 在创建数据库文件后将成为持久化格式参数，未来存储层需验证与文件格式兼容。

加载顺序：内置默认值 → 指定 JSON 文件的字段 → 已设置的环境变量 → 显式命令行选项 → 最终数值与组合验证。每一层都检查值的格式；因此低优先级配置中写错类型或环境变量格式也会报错，不会被后续层悄悄掩盖。

指定文件格式不合法、存在未知字段、字段类型错误、存在尾随 JSON/垃圾字符都会报告配置错误。未指定文件时直接使用内置默认值。

## 环境变量与参数映射

| 环境变量 | 命令行参数 | 对应配置 |
|---|---|---|
| MINISQL_CONFIG | --config / -c | 配置文件路径 |
| MINISQL_DATA_DIRECTORY | --data | data_directory |
| MINISQL_WAL_DIRECTORY | --wal-dir | wal_directory |
| MINISQL_CATALOG_DIRECTORY | --catalog-dir | catalog_directory |
| MINISQL_PAGE_SIZE | --page-size | page_size |
| MINISQL_BUFFER_POOL_SIZE | --buffer-pool-size | buffer_pool_size |
| MINISQL_REPLACEMENT_POLICY | --replacement-policy | replacement_policy |
| MINISQL_MODE | --mode | mode |
| MINISQL_HOST | --host | server.host |
| MINISQL_PORT | --port | server.port |
| MINISQL_WEBSOCKET_ENABLED | --no-websocket（置 false） | server.websocket_enabled |
| MINISQL_LOG_LEVEL | --log-level | logging.level |
| MINISQL_LOG_DIRECTORY | --log-dir | logging.directory |
| MINISQL_LOG_CONSOLE | --no-console（置 false） | logging.console |
| MINISQL_LOG_MAX_FILE_SIZE | --log-max-file-size | logging.max_file_size |
| MINISQL_LOG_MAX_FILES | --log-max-files | logging.max_files |

布尔环境变量只接受 `true`、`false`、`1`、`0`。整数必须是完整数字字符串，不接受小数和带尾随字符的值。环境读取函数通过接口注入，测试不会修改进程全局环境变量。

## 错误与返回值

`MiniSqlError` 继承 `std::runtime_error`，额外包含 `ErrorCode`、`SourceLocation` 和修复建议。行列从 1 开始，未知位置用 0。序列化格式：

```json
{
  "success": false,
  "error": {
    "type": "SyntaxError",
    "code": 2002,
    "message": "Expected FROM",
    "line": 2,
    "column": 7,
    "suggestion": "Add FROM before table"
  }
}
```

| 代码 | 分类 |
|---|---|
| 1001 / 1002 | 参数 / 配置 |
| 2001 / 2002 / 2003 | 词法 / 语法 / 语义 |
| 3001 / 4001 / 5001 | Catalog / 存储 / 执行 |
| 6001 / 7001 / 8001 | 事务 / 权限 / 网络 |
| 9001 / 9999 | 未实现 / 内部异常 |

CLI 最外层捕获业务异常并输出 JSON。未知标准异常转换为通用内部错误，不回显任意底层异常内容。CLI11 的帮助和参数格式错误通过 CLI11 原生文本输出。

`Status{}` 表示成功；错误状态可以调用 `throwIfError()`。`Result<T>` 只能存放值或失败状态，禁止用成功 Status 构建一个无值 Result；对失败结果取值会抛出 `MiniSqlError`。避免在同一个函数中混用多种失败约定。

## 日志接口

```cpp
minisql::LogManager logs(config.logging);
logs.log("storage", spdlog::level::info, "Page fetched", {"request-1", "txn-8"});
logs.flush();
```

初始化时一次性创建模块 logger，不使用 spdlog 全局注册表。构造后可以由多个工作线程并发调用 `log()`；销毁前调用方必须先停止这些线程。不同 LogManager 实例需使用不同目录，避免多个文件轮转器操作同一文件。

一条日志包含时间、级别、模块、线程 ID、请求 ID、事务 ID 和消息。模块名必须来自已注册列表，错误模块名抛出参数异常。多行和控制字符会被处理为单行；日志调用方负责排除密码、Token 和敏感内容。

控制台输出到 stderr；每个模块有独立轮转文件。错误级别触发 flush，正常退出显式 flush，析构兜底 flush。flush 不等同于 fsync，诊断日志不承担事务恢复职责。

## 测试策略

GoogleTest 覆盖默认值与配置覆盖顺序、配置边界与坏输入、Unicode 文件路径、目录冲突、日志过滤、轮转、并发写入、诊断序列化、Status/Result 不变量及 CLI 行为。

CTest 额外启动真实 `minisql` 可执行程序，检查版本、配置加载、配置 JSON、错误退出和依赖 DLL 可加载性。测试临时目录由测试夹具唯一创建，并只清理自己创建的目录。

新增测试时直接扩展对应测试文件，或在 `tests/CMakeLists.txt` 增加源文件。配置参数化用例由 GoogleTest 自动发现并注册到 CTest。
