#pragma once
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <variant>
#include <nlohmann/json.hpp>

namespace minisql {
struct CommandLineOptions {
// 命令行解析结果：把 --config、--set、--check、--print、--execute 等参数整理成结构。
    std::optional<std::filesystem::path> configFile;
    // 配置文件路径，没写就是空。
    nlohmann::json overrides = nlohmann::json::object();
    // 命令行覆盖项，形如 a.b=c，最终合并进配置。
    bool checkConfig{false};
    // 是否只检查配置合法性然后退出。
    bool printConfig{false};
    // 是否打印最终生效的配置然后退出。
    std::optional<std::string> execute;
    // 是否在启动时直接执行一条 SQL。
};
// int represents an early exit (help/version/parser error).
// int 表示提前退出（帮助、版本、参数解析错误等），不是正常返回，所以用 variant 区分两种情况。
std::variant<CommandLineOptions, int> parseCommandLine(int argc, const char* const* argv,
// 解析命令行：成功返回选项结构，需要提前退出时返回退出码。
                                                     std::ostream& out, std::ostream& err);
                                                     // （承接上一行）out / err 用于向用户打印帮助或错误信息。
} // namespace minisql
