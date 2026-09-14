#pragma once
#include "minisql/common/filesystem.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace minisql {
struct LogConfig {
// 日志配置。
    std::string level{"INFO"};
    // 日志级别，默认 INFO；取值由 validate 检查。
    std::string directory{"./logs"};
    // 日志目录，默认工作目录下的 logs。
    bool console{true};
    // 是否同时打印到控制台。
    std::size_t maxFileSize{5 * 1024 * 1024};
    // 单个日志文件超过这个字节数就轮转，默认 5 MB。
    std::size_t maxFiles{3};
    // 保留的历史日志文件个数。
};
struct ServerConfig {
// 服务端配置。
    std::string host{"127.0.0.1"};
    // 监听地址，默认只允许本机访问。
    int port{8080};
    // 监听端口。
    bool websocketEnabled{true};
    // 是否启用 WebSocket 能力开关。
};
struct Config {
// 顶层配置结构。
    std::string dataDirectory{"./data"};
    // 数据文件目录。
    std::string walDirectory{"./data/wal"};
    // 预写日志目录。
    std::string catalogDirectory{"./data/system"};
    // 系统目录文件目录。
    std::size_t pageSize{4096};
    // 页大小，默认 4096 字节，必须是 2 的幂。
    std::size_t bufferPoolSize{128};
    // 缓冲池容量，单位是页。
    std::string replacementPolicy{"LRU"};
    // 缓存替换策略，只能是 LRU 或 FIFO。
    std::string mode{"cli"};
    // 运行模式，只能是 cli 或 server。
    ServerConfig server;
    // 嵌套的服务端配置。
    LogConfig logging;
    // 嵌套的日志配置。
    nlohmann::json toJson() const;
    // 把当前配置导出成 JSON；它同时充当校验用的结构模板。
    void validate() const;
    // 校验各字段的取值范围，不合法时抛配置错误。
};

using Environment = std::function<std::optional<std::string>(const std::string&)>;
// 环境变量读取器的类型：输入变量名，返回可能为空的字符串。
std::optional<std::string> processEnvironment(const std::string& name);
// 默认实现，真的去读进程环境变量；测试时可以换成假的实现。

// Relative runtime paths are resolved against the process working directory.
// No filesystem writes occur here. Precedence: CLI > environment > file > defaults.
Config loadConfig(const std::optional<std::filesystem::path>& file = std::nullopt,
                  const nlohmann::json& overrides = nlohmann::json::object(),
                  const Environment& environment = processEnvironment);
// 配置装载入口：file 是配置文件，overrides 是命令行覆盖，environment 是环境变量读取器。
// 优先级固定为 命令行 > 环境变量 > 配置文件 > 默认值；本函数不写任何文件。
void prepareRuntimeDirectories(const Config& config);
// 按配置创建数据、日志、目录等运行时目录。
} // namespace minisql
