#pragma once
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <variant>
#include <nlohmann/json.hpp>

namespace minisql {
struct CommandLineOptions {
    std::optional<std::filesystem::path> configFile;
    nlohmann::json overrides = nlohmann::json::object();
    bool checkConfig{false};
    bool printConfig{false};
    std::optional<std::string> execute;
};
// int represents an early exit (help/version/parser error).
std::variant<CommandLineOptions, int> parseCommandLine(int argc, const char* const* argv,
                                                     std::ostream& out, std::ostream& err);
} // namespace minisql
