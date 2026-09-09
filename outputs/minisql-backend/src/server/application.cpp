#include "minisql/server/application.hpp"
#include "minisql/server/command_line.hpp"
#include "minisql/common/error.hpp"
#include "minisql/common/logger.hpp"
#include "minisql/common/version.hpp"
#include <iostream>
#include <fmt/format.h>

namespace minisql {
namespace {
MiniSqlError unavailable(const std::string& feature) {
    return {ErrorCode::NotImplemented, feature + " is not implemented in the foundation stage", {},
            "Use --check-config, --print-config, or .help in CLI mode."};
}
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return {}; }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
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
        if (options.execute) { throw unavailable("SQL execution"); }
        if (config.mode == "server") { throw unavailable("HTTP/WebSocket server"); }

        prepareRuntimeDirectories(config);
        LogManager logs(config.logging);
        logs.log("server", spdlog::level::info, "MiniSQL foundation starting", {"startup", "-"});
        output << fmt::format("MiniSQL {} | mode=cli | SQL engine: not implemented\n", kVersion);
        output << "Type .help for commands; .quit to exit.\n";
        std::string line;
        while (output << "minisql> " << std::flush, std::getline(input, line)) {
            const auto command = trim(line);
            if (command.empty()) { continue; }
            if (command == ".quit" || command == ".exit") { break; }
            if (command == ".help") { output << ".help  .version  .config  .quit  .exit\nSQL execution is not implemented yet.\n"; }
            else if (command == ".version") { output << kVersion << '\n'; }
            else if (command == ".config") { output << config.toJson().dump(2) << '\n'; }
            else {
                // Do not log raw SQL: it can contain passwords and user data.
                logs.log("sql", spdlog::level::warn, "Query rejected: SQL engine unavailable");
                error << unavailable("SQL execution").toJson().dump() << '\n';
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
