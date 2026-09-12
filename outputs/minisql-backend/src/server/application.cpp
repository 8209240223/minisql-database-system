#include "minisql/server/application.hpp"
#include "minisql/server/command_line.hpp"
#include "minisql/common/error.hpp"
#include "minisql/common/logger.hpp"
#include "minisql/common/version.hpp"
#include "minisql/common/wire_json.hpp"
#include "minisql/execution/database.hpp"
#include <iostream>
#include <fmt/format.h>

namespace minisql {
namespace {
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return {}; }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::filesystem::path databasePath(const Config& config) {
    return pathFromUtf8(config.dataDirectory) / "minisql.pages";
}
void writeResult(std::ostream& output, nlohmann::json result) {
    result["integerEncoding"] = "safe-number-or-decimal-string";
    output << wireJson(std::move(result)).dump() << '\n';
}
} // namespace

int runApplication(int argc, const char* const* argv, std::istream& input,
                   std::ostream& output, std::ostream& error, const Environment& environment) {
    try {
        auto parsed = parseCommandLine(argc, argv, output, error);
        if (const auto* exitCode = std::get_if<int>(&parsed)) { return *exitCode; }
        const auto& options = std::get<CommandLineOptions>(parsed);
        auto configFile = options.configFile;
        if (!configFile) {
            if (auto path = environment("MINISQL_CONFIG")) { configFile = pathFromUtf8(*path); }
        }
        const auto config = loadConfig(configFile, options.overrides, environment);
        if (options.printConfig) { output << config.toJson().dump(2) << '\n'; return 0; }
        if (options.checkConfig) { output << "Configuration valid\n"; return 0; }
        prepareRuntimeDirectories(config);
        LogManager logs(config.logging);
        logs.log("server", spdlog::level::info, "MiniSQL starting", {"startup", "-"});
        if (config.mode == "server") {
            throw MiniSqlError(ErrorCode::NotImplemented,
                "Native HTTP/WebSocket server is not available; use scripts/database-bridge.mjs");
        }
        execution::Database database(databasePath(config), config.bufferPoolSize);
        if (options.execute) {
            const auto result = database.executeScript(*options.execute);
            writeResult(output, result);
            return result.value("success", false) ? 0 : 1;
        }

        output << fmt::format("MiniSQL {} | mode=cli\n", kVersion);
        output << "Type .help for commands; .quit to exit.\n";
        std::string line;
        while (output << "minisql> " << std::flush, std::getline(input, line)) {
            const auto command = trim(line);
            if (command.empty()) { continue; }
            if (command == ".quit" || command == ".exit") { break; }
            if (command == ".help") { output << ".help  .version  .config  .quit  .exit\nEnter SQL terminated by a semicolon to execute it.\n"; }
            else if (command == ".version") { output << kVersion << '\n'; }
            else if (command == ".config") { output << config.toJson().dump(2) << '\n'; }
            else {
                const auto result = database.execute(command);
                writeResult(result.value("success", false) ? output : error, result);
            }
        }
        if (input.bad() || (input.fail() && !input.eof())) {
            throw MiniSqlError(ErrorCode::InvalidArgument, "Failed to read CLI input");
        }
        logs.log("server", spdlog::level::info, "MiniSQL foundation stopped", {"shutdown", "-"});
        logs.flush();
        return 0;
    } catch (const MiniSqlError& exception) {
        error << exception.toJson().dump() << '\n';
        return 1;
    } catch (const std::exception&) {
        // Avoid leaking implementation details or sensitive exception payloads.
        error << MiniSqlError(ErrorCode::Internal, "Unexpected internal failure").toJson().dump() << '\n';
        return 1;
    }
}
} // namespace minisql
