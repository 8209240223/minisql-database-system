#include "minisql/server/command_line.hpp"
#include "minisql/common/filesystem.hpp"
#include "minisql/common/version.hpp"
#include <CLI/CLI.hpp>

namespace minisql {
std::variant<CommandLineOptions, int> parseCommandLine(int argc, const char* const* argv,
// 解析命令行：正常返回选项结构，需要提前退出时返回退出码。
                                                     std::ostream& out, std::ostream& err) {
// out 用于打印帮助与版本，err 用于打印解析错误。
    CLI::App app{"MiniSQL database command-line client"};
    app.set_version_flag("--version", std::string(kVersion));
    // 注册 --version；命中后 CLI11 直接打印版本并退出。
    CommandLineOptions options;
    // 结果结构。
    std::string config, sql;
    // 两个原始字符串缓冲，等解析成功后再决定是否写入 options。
    auto* configOption = app.add_option("-c,--config", config, "JSON config file (explicit paths must exist)");
    // 配置文件路径。显式给出的路径必须存在，否则由后续配置加载阶段报错。
    auto* executeOption = app.add_option("-e,--execute", sql, "Execute SQL and exit");
    app.add_flag("--check-config", options.checkConfig, "Validate configuration and exit without writing files");
    // 只校验配置：不写文件、不建目录。
    app.add_flag("--print-config", options.printConfig, "Print effective JSON configuration and exit without writing files");
    // 打印最终生效的配置：同样不写文件，方便排查优先级问题。
    // The callback only runs for supplied options, preserving lower priority values.
    // 回调只在"用户真的给了这个选项"时才执行，因此没给的项不会被写入覆盖表，
    // 这样低优先级的来源（配置文件、环境变量、默认值）才有机会生效。

    // The callback only runs for supplied options, preserving lower priority values.
    auto stringOption = [&](const char* flags, const char* pointer, const char* description) {
    // 注册一个字符串选项。pointer 是 JSON 指针，指明这个值写进配置的哪个位置。
        app.add_option_function<std::string>(flags, [&, pointer](const std::string& value) {
        // 只有在选项出现时才回调；值以字符串形式传入。
            options.overrides[nlohmann::json::json_pointer(pointer)] = value;
            // 按 JSON 指针把覆盖值写进 overrides，后续与配置文件做深合并。
        }, description);
        // 注册结束，description 会出现在 --help 里。
    };
    // 字符串选项工厂结束。
    auto integerOption = [&](const char* flags, const char* pointer, const char* description) {
    // 注册一个整数选项，结构与字符串版本一致。
        app.add_option_function<std::int64_t>(flags, [&, pointer](std::int64_t value) {
        // 用 64 位整数接收，避免大数值溢出。
            options.overrides[nlohmann::json::json_pointer(pointer)] = value;
            // 同样按 JSON 指针写进覆盖表。
        }, description);
        // 注册结束。
    };
    // 整数选项工厂结束。
    stringOption("--mode", "/mode", "cli or server");
    stringOption("--data", "/data_directory", "Data directory");
    // 数据目录。
    stringOption("--wal-dir", "/wal_directory", "WAL directory");
    // 预写日志目录。
    stringOption("--catalog-dir", "/catalog_directory", "Catalog directory");
    // 目录文件目录。
    stringOption("--host", "/server/host", "Server bind host");
    // server 模式的监听地址。
    integerOption("--port", "/server/port", "Server port: 1-65535");
    // server 模式的监听端口。
    integerOption("--page-size", "/page_size", "Power of two: 512-65536 bytes");
    // 页大小，必须是 512 到 65536 之间的 2 的幂。
    integerOption("--buffer-pool-size", "/buffer_pool_size", "Buffer pool capacity in pages");
    // 缓冲池容量，单位是页。
    stringOption("--replacement-policy", "/replacement_policy", "LRU or FIFO");
    // 缓冲池淘汰策略。
    stringOption("--log-level", "/logging/level", "TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL, OFF");
    // 日志级别。
    stringOption("--log-dir", "/logging/directory", "Log directory");
    // 日志目录。
    integerOption("--log-max-file-size", "/logging/max_file_size", "Rotation threshold in bytes");
    // 单个日志文件的轮转阈值（字节）。
    integerOption("--log-max-files", "/logging/max_files", "Number of retained rotated files");
    // 保留的历史日志文件个数。
    bool noConsole = false;
    // --no-console 的落点：解析成功后据此写覆盖表。
    bool noWebsocket = false;
    // --no-websocket 的落点。
    app.add_flag("--no-console", noConsole, "Disable console logging (file logs remain enabled)");
    // 关闭控制台日志；文件日志不受影响。
    app.add_flag("--no-websocket", noWebsocket, "Disable configured WebSocket capability");
    // 关闭配置里启用的 WebSocket 能力。
    try { app.parse(argc, argv); }
    // 真正解析参数；帮助与版本会在这里触发 CLI11 的提前退出异常。
    catch (const CLI::ParseError& error) { return app.exit(error, out, err); }
    // 捕获解析错误（含 --help/--version），交给 CLI11 打印并取得退出码返回。
    if (configOption->count()) { options.configFile = pathFromUtf8(config); }
    // 只有当用户确实写了 --config 才写入结果，避免空串被误当成路径。
    if (executeOption->count()) { options.execute = sql; }
    // 同理，只有写了 -e/--execute 才写入 SQL。
    if (noConsole) { options.overrides["logging"]["console"] = false; }
    // --no-console 转成配置覆盖项，走与其它选项一致的合并路径。
    if (noWebsocket) { options.overrides["server"]["websocket_enabled"] = false; }
    // --no-websocket 同理。
    return options;
    // 解析成功，返回选项结构。
}
} // namespace minisql
