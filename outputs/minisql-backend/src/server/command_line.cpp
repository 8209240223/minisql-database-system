#include "minisql/server/command_line.hpp"
#include "minisql/common/filesystem.hpp"
#include "minisql/common/version.hpp"
#include <CLI/CLI.hpp>

namespace minisql {
std::variant<CommandLineOptions, int> parseCommandLine(int argc, const char* const* argv,
                                                     std::ostream& out, std::ostream& err) {
    CLI::App app{"MiniSQL foundation: configuration, logging and CLI shell"};
    app.set_version_flag("--version", std::string(kVersion));
    CommandLineOptions options;
    std::string config, sql;
    auto* configOption = app.add_option("-c,--config", config, "JSON config file (explicit paths must exist)");
    auto* executeOption = app.add_option("-e,--execute", sql, "Submit SQL (engine not implemented yet)");
    app.add_flag("--check-config", options.checkConfig, "Validate configuration and exit without writing files");
    app.add_flag("--print-config", options.printConfig, "Print effective JSON configuration and exit without writing files");

    // The callback only runs for supplied options, preserving lower priority values.
    auto stringOption = [&](const char* flags, const char* pointer, const char* description) {
        app.add_option_function<std::string>(flags, [&, pointer](const std::string& value) {
            options.overrides[nlohmann::json::json_pointer(pointer)] = value;
        }, description);
    };
    auto integerOption = [&](const char* flags, const char* pointer, const char* description) {
        app.add_option_function<std::int64_t>(flags, [&, pointer](std::int64_t value) {
            options.overrides[nlohmann::json::json_pointer(pointer)] = value;
        }, description);
    };
    stringOption("--mode", "/mode", "cli or server (network server is not implemented yet)");
    stringOption("--data", "/data_directory", "Data directory");
    stringOption("--wal-dir", "/wal_directory", "WAL directory");
    stringOption("--catalog-dir", "/catalog_directory", "Catalog directory");
    stringOption("--host", "/server/host", "Server bind host");
    integerOption("--port", "/server/port", "Server port: 1-65535");
    integerOption("--page-size", "/page_size", "Power of two: 512-65536 bytes");
    integerOption("--buffer-pool-size", "/buffer_pool_size", "Buffer pool capacity in pages");
    stringOption("--replacement-policy", "/replacement_policy", "LRU or FIFO");
    stringOption("--log-level", "/logging/level", "TRACE, DEBUG, INFO, WARN, ERROR, CRITICAL, OFF");
    stringOption("--log-dir", "/logging/directory", "Log directory");
    integerOption("--log-max-file-size", "/logging/max_file_size", "Rotation threshold in bytes");
    integerOption("--log-max-files", "/logging/max_files", "Number of retained rotated files");
    bool noConsole = false;
    bool noWebsocket = false;
    app.add_flag("--no-console", noConsole, "Disable console logging (file logs remain enabled)");
    app.add_flag("--no-websocket", noWebsocket, "Disable configured WebSocket capability");
    try { app.parse(argc, argv); }
    catch (const CLI::ParseError& error) { return app.exit(error, out, err); }
    if (configOption->count()) { options.configFile = pathFromUtf8(config); }
    if (executeOption->count()) { options.execute = sql; }
    if (noConsole) { options.overrides["logging"]["console"] = false; }
    if (noWebsocket) { options.overrides["server"]["websocket_enabled"] = false; }
    return options;
}
} // namespace minisql
