#include "minisql/common/config.hpp"
#include "minisql/common/error.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#endif

namespace minisql {
namespace {
using Json = nlohmann::json;
[[noreturn]] void configError(const std::string& message) {
    throw MiniSqlError(ErrorCode::Configuration, message, {},
                       "Check config keys, value types and allowed ranges.");
}

// Validate before merge_patch: null must not silently remove a required key.
void checkShape(const Json& patch, const Json& schema, const std::string& path = "config") {
    if (!patch.is_object()) { configError(path + " must be an object"); }
    for (const auto& [key, value] : patch.items()) {
        const auto field = path + "." + key;
        if (!schema.contains(key)) { configError("Unknown key: " + field); }
        const auto& expected = schema.at(key);
        if (expected.is_object()) { checkShape(value, expected, field); }
        else if ((expected.is_string() && !value.is_string()) ||
                 (expected.is_boolean() && !value.is_boolean()) ||
                 (expected.is_number_integer() && !value.is_number_integer())) {
            configError("Invalid type for " + field);
        }
    }
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::uint64_t boundedInteger(const Json& j, const std::string& field,
                             std::uint64_t low, std::uint64_t high) {
    if (!j.is_number_integer() || (j.is_number_integer() && !j.is_number_unsigned() && j.get<std::int64_t>() < 0)) {
        configError(field + " must be a non-negative integer");
    }
    auto value = j.get<std::uint64_t>();
    if (value < low || value > high) { configError(field + " is out of range"); }
    return value;
}
} // namespace

nlohmann::json Config::toJson() const {
    return {{"data_directory", dataDirectory}, {"wal_directory", walDirectory},
        {"catalog_directory", catalogDirectory}, {"page_size", pageSize},
        {"buffer_pool_size", bufferPoolSize}, {"replacement_policy", replacementPolicy},
        {"mode", mode},
        {"server", {{"host", server.host}, {"port", server.port},
                    {"websocket_enabled", server.websocketEnabled}}},
        {"logging", {{"level", logging.level}, {"directory", logging.directory},
                     {"console", logging.console}, {"max_file_size", logging.maxFileSize},
                     {"max_files", logging.maxFiles}}}};
}

void Config::validate() const {
    for (const auto* path : {&dataDirectory, &walDirectory, &catalogDirectory, &logging.directory}) {
        if (path->empty() || path->find('\0') != std::string::npos) { configError("Directory path is empty or contains NUL"); }
    }
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) != 0) {
        configError("page_size must be a power of two between 512 and 65536");
    }
    if (bufferPoolSize == 0 || bufferPoolSize > 1048576) { configError("buffer_pool_size must be in [1, 1048576] pages"); }
    if (replacementPolicy != "LRU" && replacementPolicy != "FIFO") { configError("replacement_policy must be LRU or FIFO"); }
    if (mode != "cli" && mode != "server") { configError("mode must be cli or server"); }
    if (server.port < 1 || server.port > 65535) { configError("server.port must be in [1, 65535]"); }
    if (server.host.empty() || server.host.find('\0') != std::string::npos) { configError("server.host must be nonempty and contain no NUL"); }
    const std::set<std::string> levels{"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF"};
    if (!levels.contains(logging.level)) { configError("Unknown logging.level"); }
    if (logging.maxFileSize < 1024 || logging.maxFileSize > 1073741824) { configError("logging.max_file_size must be in [1024, 1073741824] bytes"); }
    if (logging.maxFiles < 1 || logging.maxFiles > 100) { configError("logging.max_files must be in [1, 100]"); }
}

std::optional<std::string> processEnvironment(const std::string& name) {
#ifdef _WIN32
    wchar_t* buffer = nullptr;
    std::size_t length = 0;
    const std::wstring wideName(name.begin(), name.end()); // Config variable names are ASCII.
    if (_wdupenv_s(&buffer, &length, wideName.c_str()) != 0 || buffer == nullptr) { return std::nullopt; }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) { std::free(buffer); configError("Invalid Unicode environment variable: " + name); }
    std::string value(static_cast<std::size_t>(size), '\0');
    const int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, value.data(), size, nullptr, nullptr);
    std::free(buffer);
    if (converted <= 0) { configError("Cannot decode environment variable: " + name); }
    value.pop_back();
    return value;
#else
    if (const char* value = std::getenv(name.c_str())) { return std::string(value); }
    return std::nullopt;
#endif
}

Config loadConfig(const std::optional<std::filesystem::path>& file,
                  const Json& overrides, const Environment& environment) {
    const auto schema = Config{}.toJson();
    auto merged = schema;
    if (file) {
        std::ifstream input(*file, std::ios::binary);
        if (!input) { configError("Cannot open config file: " + pathToUtf8(*file)); }
        Json patch;
        try { input >> patch; }
        catch (const Json::exception&) { configError("Malformed JSON in config file: " + pathToUtf8(*file)); }
        input >> std::ws;
        if (!input.eof()) { configError("Trailing content in config file"); }
        checkShape(patch, schema);
        merged.merge_patch(patch);
    }
    // Each entry maps an environment name to a JSON pointer in the schema.
    const std::pair<const char*, const char*> bindings[] = {
        {"MINISQL_DATA_DIRECTORY", "/data_directory"}, {"MINISQL_WAL_DIRECTORY", "/wal_directory"},
        {"MINISQL_CATALOG_DIRECTORY", "/catalog_directory"}, {"MINISQL_PAGE_SIZE", "/page_size"},
        {"MINISQL_BUFFER_POOL_SIZE", "/buffer_pool_size"}, {"MINISQL_REPLACEMENT_POLICY", "/replacement_policy"},
        {"MINISQL_MODE", "/mode"}, {"MINISQL_HOST", "/server/host"}, {"MINISQL_PORT", "/server/port"},
        {"MINISQL_WEBSOCKET_ENABLED", "/server/websocket_enabled"}, {"MINISQL_LOG_LEVEL", "/logging/level"},
        {"MINISQL_LOG_DIRECTORY", "/logging/directory"}, {"MINISQL_LOG_CONSOLE", "/logging/console"},
        {"MINISQL_LOG_MAX_FILE_SIZE", "/logging/max_file_size"}, {"MINISQL_LOG_MAX_FILES", "/logging/max_files"}
    };
    for (const auto& [name, pointer] : bindings) {
        auto raw = environment(name);
        if (!raw) { continue; }
        const Json::json_pointer key(pointer);
        const auto& expected = schema.at(key);
        if (expected.is_string()) { merged[key] = *raw; }
        else if (expected.is_boolean()) {
            if (*raw == "true" || *raw == "1") { merged[key] = true; }
            else if (*raw == "false" || *raw == "0") { merged[key] = false; }
            else { configError(std::string(name) + " must be true, false, 1 or 0"); }
        } else {
            std::int64_t number{};
            const auto [end, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), number);
            if (ec != std::errc{} || end != raw->data() + raw->size()) {
                configError(std::string(name) + " must be an integer");
            }
            merged[key] = number;
        }
    }
    checkShape(overrides, schema);
    merged.merge_patch(overrides);
    Config result;
    result.dataDirectory = merged.at("data_directory").get<std::string>();
    result.walDirectory = merged.at("wal_directory").get<std::string>();
    result.catalogDirectory = merged.at("catalog_directory").get<std::string>();
    result.pageSize = static_cast<std::size_t>(boundedInteger(merged.at("page_size"), "page_size", 512, 65536));
    result.bufferPoolSize = static_cast<std::size_t>(boundedInteger(merged.at("buffer_pool_size"), "buffer_pool_size", 1, 1048576));
    result.replacementPolicy = uppercase(merged.at("replacement_policy").get<std::string>());
    result.mode = merged.at("mode").get<std::string>();
    const auto& server = merged.at("server");
    result.server.host = server.at("host").get<std::string>();
    result.server.port = static_cast<int>(boundedInteger(server.at("port"), "server.port", 1, 65535));
    result.server.websocketEnabled = server.at("websocket_enabled").get<bool>();
    const auto& logging = merged.at("logging");
    result.logging.level = uppercase(logging.at("level").get<std::string>());
    result.logging.directory = logging.at("directory").get<std::string>();
    result.logging.console = logging.at("console").get<bool>();
    result.logging.maxFileSize = static_cast<std::size_t>(boundedInteger(logging.at("max_file_size"), "logging.max_file_size", 1024, 1073741824));
    result.logging.maxFiles = static_cast<std::size_t>(boundedInteger(logging.at("max_files"), "logging.max_files", 1, 100));
    result.validate();
    return result;
}

void prepareRuntimeDirectories(const Config& config) {
    config.validate();
    for (const auto& path : {config.dataDirectory, config.walDirectory,
                             config.catalogDirectory, config.logging.directory}) {
        std::error_code ec;
        std::filesystem::create_directories(pathFromUtf8(path), ec);
        if (ec) { throw MiniSqlError(ErrorCode::Storage, "Cannot create runtime directory: " + path, {}, "Check the path and write permissions."); }
    }
}
} // namespace minisql
