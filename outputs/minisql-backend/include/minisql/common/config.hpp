#pragma once
#include "minisql/common/filesystem.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace minisql {
struct LogConfig {
    std::string level{"INFO"};
    std::string directory{"./logs"};
    bool console{true};
    std::size_t maxFileSize{5 * 1024 * 1024};
    std::size_t maxFiles{3};
};
struct ServerConfig {
    std::string host{"127.0.0.1"};
    int port{8080};
    bool websocketEnabled{true};
};
struct Config {
    std::string dataDirectory{"./data"};
    std::string walDirectory{"./data/wal"};
    std::string catalogDirectory{"./data/system"};
    std::size_t pageSize{4096};
    std::size_t bufferPoolSize{128};
    std::string replacementPolicy{"LRU"};
    std::string mode{"cli"};
    ServerConfig server;
    LogConfig logging;
    nlohmann::json toJson() const;
    void validate() const;
};

using Environment = std::function<std::optional<std::string>(const std::string&)>;
std::optional<std::string> processEnvironment(const std::string& name);

// Relative runtime paths are resolved against the process working directory.
// No filesystem writes occur here. Precedence: CLI > environment > file > defaults.
Config loadConfig(const std::optional<std::filesystem::path>& file = std::nullopt,
                  const nlohmann::json& overrides = nlohmann::json::object(),
                  const Environment& environment = processEnvironment);
void prepareRuntimeDirectories(const Config& config);
} // namespace minisql
