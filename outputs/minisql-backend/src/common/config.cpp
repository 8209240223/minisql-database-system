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
// 给 nlohmann::json 起个短名字，减少书写量。
[[noreturn]] void configError(const std::string& message) {
// 配置错误的统一出口；[[noreturn]] 告诉编译器这个函数一定不会正常返回。
    throw MiniSqlError(ErrorCode::Configuration, message, {},
                       "Check config keys, value types and allowed ranges.");
    // 抛出配置类错误，并附带一条固定的修复建议。
}

// Validate before merge_patch: null must not silently remove a required key.
void checkShape(const Json& patch, const Json& schema, const std::string& path = "config") {
// 在合并之前检查用户给的键名与类型是否与模板一致。
    if (!patch.is_object()) { configError(path + " must be an object"); }
    // 顶层必须是一个 JSON 对象，否则直接报错。
    for (const auto& [key, value] : patch.items()) {
    // 逐个遍历用户提供的键值对。
        const auto field = path + "." + key;
        // 拼出带层级的字段名，便于报错时定位。
        if (!schema.contains(key)) { configError("Unknown key: " + field); }
        // 模板里没有这个键，说明写错了配置项名。
        const auto& expected = schema.at(key);
        // 取出模板中该键的期望类型。
        if (expected.is_object()) { checkShape(value, expected, field); }
        // 期望是对象，说明还有下一层，递归检查。
        else if ((expected.is_string() && !value.is_string()) ||
                 (expected.is_boolean() && !value.is_boolean()) ||
                 (expected.is_number_integer() && !value.is_number_integer())) {
            // 三个基本类型逐个比对：字符串、布尔、整数。
            configError("Invalid type for " + field);
            // 类型不匹配就报错并指出字段名。
        }
    }
}

std::string uppercase(std::string value) {
// 把字符串整体转成大写，用于日志级别与替换策略的归一化。
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    // 逐字符转大写；入参用 unsigned char 避免负值传给 toupper 造成未定义行为。
    return value;
    // 返回转换后的副本。
}

std::uint64_t boundedInteger(const Json& j, const std::string& field,
                             std::uint64_t low, std::uint64_t high) {
// 取值并做范围检查，返回非负整数。
    if (!j.is_number_integer() || (j.is_number_integer() && !j.is_number_unsigned() && j.get<std::int64_t>() < 0)) {
        configError(field + " must be a non-negative integer");
        // 不是整数，或者是负数，都算不合法。
    }
    auto value = j.get<std::uint64_t>();
    // 到这里可以安全地取出无符号整数。
    if (value < low || value > high) { configError(field + " is out of range"); }
    // 超出该字段允许的范围就报错。
    return value;
    // 返回通过检查的数值。
}
} // namespace

nlohmann::json Config::toJson() const {
// 把配置导出成 JSON；它同时被当作配置模板（schema）使用。
    return {{"data_directory", dataDirectory}, {"wal_directory", walDirectory},
        // 数据目录与 WAL 目录两个字段。
        {"catalog_directory", catalogDirectory}, {"page_size", pageSize},
        // 系统目录位置与页大小。
        {"buffer_pool_size", bufferPoolSize}, {"replacement_policy", replacementPolicy},
        // 缓冲池容量与替换策略。
        {"mode", mode},
        // 运行模式。
        {"server", {{"host", server.host}, {"port", server.port},
                    {"websocket_enabled", server.websocketEnabled}}},
        // 嵌套的服务端配置对象。
        {"logging", {{"level", logging.level}, {"directory", logging.directory},
                     {"console", logging.console}, {"max_file_size", logging.maxFileSize},
                     {"max_files", logging.maxFiles}}}};
        // 嵌套的日志配置对象。
}

void Config::validate() const {
// 逐项校验取值是否合法，任何一个不合法都会抛配置错误。
    for (const auto* path : {&dataDirectory, &walDirectory, &catalogDirectory, &logging.directory}) {
    // 遍历四个目录字段。
        if (path->empty() || path->find('\0') != std::string::npos) { configError("Directory path is empty or contains NUL"); }
        // 目录不能为空，也不能含空字符（空字符会被操作系统截断）。
    }
    if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1)) != 0) {
    // 页大小必须在 512 到 65536 之间，并且是 2 的幂。
        configError("page_size must be a power of two between 512 and 65536");
        // 位运算与自身减一做与，结果为 0 才说明是 2 的幂。
    }
    if (bufferPoolSize == 0 || bufferPoolSize > 1048576) { configError("buffer_pool_size must be in [1, 1048576] pages"); }
    // 缓冲池至少一页，上限一百万页。
    if (replacementPolicy != "LRU" && replacementPolicy != "FIFO") { configError("replacement_policy must be LRU or FIFO"); }
    // 替换策略只能是两种之一。
    if (mode != "cli" && mode != "server") { configError("mode must be cli or server"); }
    // 运行模式只能是 cli 或 server。
    if (server.port < 1 || server.port > 65535) { configError("server.port must be in [1, 65535]"); }
    // 端口必须是有效范围。
    if (server.host.empty() || server.host.find('\0') != std::string::npos) { configError("server.host must be nonempty and contain no NUL"); }
    // 监听地址非空且不含空字符。
    const std::set<std::string> levels{"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL", "OFF"};
    // 合法的日志级别集合。
    if (!levels.contains(logging.level)) { configError("Unknown logging.level"); }
    // 不在集合里就报错。
    if (logging.maxFileSize < 1024 || logging.maxFileSize > 1073741824) { configError("logging.max_file_size must be in [1024, 1073741824] bytes"); }
    // 轮转阈值范围：1 KB 到 1 GB。
    if (logging.maxFiles < 1 || logging.maxFiles > 100) { configError("logging.max_files must be in [1, 100]"); }
    // 保留份数范围：1 到 100。
}

std::optional<std::string> processEnvironment(const std::string& name) {
// 默认的环境变量读取实现：读不到就返回空。
#ifdef _WIN32
    wchar_t* buffer = nullptr;
    // 接收系统分配的内存指针。
    std::size_t length = 0;
    // 接收长度。
    const std::wstring wideName(name.begin(), name.end()); // Config variable names are ASCII.
    // 变量名本身是 ASCII，因此可以直接逐字节转宽字符。
    if (_wdupenv_s(&buffer, &length, wideName.c_str()) != 0 || buffer == nullptr) { return std::nullopt; }
    // 读取失败或变量不存在时返回空，调用方会跳过该变量。
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, nullptr, 0, nullptr, nullptr);
    // 第一次调用只为问需要多少字节；-1 表示按以零结尾的字符串计算。
    if (size <= 0) { std::free(buffer); configError("Invalid Unicode environment variable: " + name); }
    // 转换失败要释放内存并报错。
    std::string value(static_cast<std::size_t>(size), '\0');
    // 按需要的长度准备缓冲区。
    const int converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer, -1, value.data(), size, nullptr, nullptr);
    // 第二次调用真正转换成 UTF-8。
    std::free(buffer);
    // 释放系统分配的内存。
    if (converted <= 0) { configError("Cannot decode environment variable: " + name); }
    // 转换失败同样报错。
    value.pop_back();
    // 去掉结尾多出来的那个零字节。
    return value;
    // 返回读取到的值。
#else
    if (const char* value = std::getenv(name.c_str())) { return std::string(value); }
    // 变量存在就返回它的值。
    return std::nullopt;
    // 不存在就返回空。
#endif
}

Config loadConfig(const std::optional<std::filesystem::path>& file,
                  const Json& overrides, const Environment& environment) {
// 配置装载主流程，优先级：命令行覆盖 > 环境变量 > 配置文件 > 默认值。
    const auto schema = Config{}.toJson();
    // 一份默认配置，同时充当校验模板。
    auto merged = schema;
    // 合并结果从默认值开始。
    if (file) {
    // 只有在给了配置文件路径时才读取文件。
        std::ifstream input(*file, std::ios::binary);
        // 以二进制方式打开，避免换行符被转换。
        if (!input) { configError("Cannot open config file: " + pathToUtf8(*file)); }
        // 打不开就报错并给出路径。
        Json patch;
        // 存放从文件解析出来的 JSON。
        try { input >> patch; }
        // 解析内容。
        catch (const Json::exception&) { configError("Malformed JSON in config file: " + pathToUtf8(*file)); }
        // 解析失败说明 JSON 语法有错。
        input >> std::ws;
        // 跳过解析之后的空白。
        if (!input.eof()) { configError("Trailing content in config file"); }
        // 若还有内容，说明文件里有多余的 JSON，属于错误。
        checkShape(patch, schema);
        // 先校验键名与类型，再合并。
        merged.merge_patch(patch);
        // 把文件里的配置合并到默认值之上。
    }
    // Each entry maps an environment name to a JSON pointer in the schema.
    const std::pair<const char*, const char*> bindings[] = {
    // 环境变量名到配置项 JSON 指针的对应表。
        {"MINISQL_DATA_DIRECTORY", "/data_directory"}, {"MINISQL_WAL_DIRECTORY", "/wal_directory"},
        // 数据目录与 WAL 目录。
        {"MINISQL_CATALOG_DIRECTORY", "/catalog_directory"}, {"MINISQL_PAGE_SIZE", "/page_size"},
        // 目录位置与页大小。
        {"MINISQL_BUFFER_POOL_SIZE", "/buffer_pool_size"}, {"MINISQL_REPLACEMENT_POLICY", "/replacement_policy"},
        // 缓冲池容量与替换策略。
        {"MINISQL_MODE", "/mode"}, {"MINISQL_HOST", "/server/host"}, {"MINISQL_PORT", "/server/port"},
        // 运行模式、监听地址与端口。
        {"MINISQL_WEBSOCKET_ENABLED", "/server/websocket_enabled"}, {"MINISQL_LOG_LEVEL", "/logging/level"},
        // WebSocket 开关与日志级别。
        {"MINISQL_LOG_DIRECTORY", "/logging/directory"}, {"MINISQL_LOG_CONSOLE", "/logging/console"},
        // 日志目录与控制台开关。
        {"MINISQL_LOG_MAX_FILE_SIZE", "/logging/max_file_size"}, {"MINISQL_LOG_MAX_FILES", "/logging/max_files"}
        // 日志轮转阈值与保留份数。
    };
    for (const auto& [name, pointer] : bindings) {
    // 逐个变量尝试读取。
        auto raw = environment(name);
        // 读取环境变量。
        if (!raw) { continue; }
        // 没设置就跳过，保持文件或默认值。
        const Json::json_pointer key(pointer);
        // 把字符串形式的指针转成真正的 JSON 指针。
        const auto& expected = schema.at(key);
        // 从模板取出该位置的期望类型，用于决定怎么解析。
        if (expected.is_string()) { merged[key] = *raw; }
        // 期望字符串就直接赋值，不做转换。
        else if (expected.is_boolean()) {
        // 期望布尔：接受 true/false 与 1/0 四种写法。
            if (*raw == "true" || *raw == "1") { merged[key] = true; }
            // 真值。
            else if (*raw == "false" || *raw == "0") { merged[key] = false; }
            // 假值。
            else { configError(std::string(name) + " must be true, false, 1 or 0"); }
            // 其他写法一律报错。
        } else {
        // 其余情况按整数解析。
            std::int64_t number{};
            const auto [end, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), number);
            // 从字符串解析整数，不做本地化处理。
            if (ec != std::errc{} || end != raw->data() + raw->size()) {
            // 解析出错，或者没有消费完整个字符串，都算非法。
                configError(std::string(name) + " must be an integer");
                // 报错并指出变量名。
            }
            merged[key] = number;
            // 写入合并结果。
        }
    }
    checkShape(overrides, schema);
    // 命令行覆盖同样先校验形状。
    merged.merge_patch(overrides);
    // 最后合并命令行覆盖，因此它优先级最高。
    Config result;
    // 准备把 JSON 还原成强类型结构。
    result.dataDirectory = merged.at("data_directory").get<std::string>();
    // 数据目录。
    result.walDirectory = merged.at("wal_directory").get<std::string>();
    // WAL 目录。
    result.catalogDirectory = merged.at("catalog_directory").get<std::string>();
    // 系统目录位置。
    result.pageSize = static_cast<std::size_t>(boundedInteger(merged.at("page_size"), "page_size", 512, 65536));
    // 页大小，带范围检查。
    result.bufferPoolSize = static_cast<std::size_t>(boundedInteger(merged.at("buffer_pool_size"), "buffer_pool_size", 1, 1048576));
    // 缓冲池容量，带范围检查。
    result.replacementPolicy = uppercase(merged.at("replacement_policy").get<std::string>());
    // 替换策略统一转大写，方便后续比较。
    result.mode = merged.at("mode").get<std::string>();
    // 运行模式。
    const auto& server = merged.at("server");
    // 取出嵌套的服务端对象。
    result.server.host = server.at("host").get<std::string>();
    // 监听地址。
    result.server.port = static_cast<int>(boundedInteger(server.at("port"), "server.port", 1, 65535));
    // 端口，带范围检查。
    result.server.websocketEnabled = server.at("websocket_enabled").get<bool>();
    // WebSocket 开关。
    const auto& logging = merged.at("logging");
    // 取出嵌套的日志对象。
    result.logging.level = uppercase(logging.at("level").get<std::string>());
    // 日志级别统一转大写。
    result.logging.directory = logging.at("directory").get<std::string>();
    // 日志目录。
    result.logging.console = logging.at("console").get<bool>();
    // 控制台输出开关。
    result.logging.maxFileSize = static_cast<std::size_t>(boundedInteger(logging.at("max_file_size"), "logging.max_file_size", 1024, 1073741824));
    // 轮转阈值，带范围检查。
    result.logging.maxFiles = static_cast<std::size_t>(boundedInteger(logging.at("max_files"), "logging.max_files", 1, 100));
    // 保留份数，带范围检查。
    result.validate();
    // 最后再整体校验一次，确保交叉约束也满足。
    return result;
    // 返回最终配置。
}

void prepareRuntimeDirectories(const Config& config) {
// 创建运行时需要的目录。
    config.validate();
    // 先校验配置，避免用非法路径去建目录。
    for (const auto& path : {config.dataDirectory, config.walDirectory,
                             config.catalogDirectory, config.logging.directory}) {
    // 四个目录逐个处理。
        std::error_code ec;
        // 接收错误码的变量；用错误码版本不会抛异常。
        std::filesystem::create_directories(pathFromUtf8(path), ec);
        // 递归创建目录，已存在时不算错误。
        if (ec) { throw MiniSqlError(ErrorCode::Storage, "Cannot create runtime directory: " + path, {}, "Check the path and write permissions."); }
        // 创建失败通常是权限或磁盘问题，抛存储类错误并给出排查建议。
    }
}
} // namespace minisql
