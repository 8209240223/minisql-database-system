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
// 构造"该功能尚未实现"的统一错误对象。
    return {ErrorCode::NotImplemented, feature + " is not implemented in the foundation stage", {},
            // 错误码取 NotImplemented，消息里带上具体功能名，便于用户定位。
            "Use --check-config, --print-config, or .help in CLI mode."};
            // 提示用户在基础阶段可以先用哪些命令，而不是直接卡死。
}
std::string trim(const std::string& text) {
// 去掉字符串首尾的空白字符，用于判断交互式输入是否为空命令。
    const auto first = text.find_first_not_of(" \t\r\n");
    // 找到第一个非空白字符的位置。
    if (first == std::string::npos) { return {}; }
    // 整个串都是空白，返回空串。
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    // 从第一个非空白字符开始，截到最后一个非空白字符（含）为止。
}
} // namespace

int runApplication(int argc, const char* const* argv, std::istream& input,
// 应用程序主入口：命令行参数、三个流与环境。
                   std::ostream& output, std::ostream& error, const Environment& environment) {
// output 用于正常输出，error 用于错误输出，environment 提供环境变量读取能力。
    try {
    // 整个入口包在 try 里，保证任何失败都转成结构化错误而不是崩溃。
        auto parsed = parseCommandLine(argc, argv, output, error);
        // 解析命令行；帮助与版本会在这里输出并给出退出码。
        if (const auto* exitCode = std::get_if<int>(&parsed)) { return *exitCode; }
        // 返回的是退出码，说明属于提前退出（帮助、版本、参数错误），直接返回它。
        const auto& options = std::get<CommandLineOptions>(parsed);
        // 否则取出正常的选项结构。
        auto configFile = options.configFile;
        // 以命令行显式指定的配置文件为起点。
        if (!configFile) {
        // 命令行没给配置文件。
            if (auto path = environment("MINISQL_CONFIG")) { configFile = pathFromUtf8(*path); }
            // 退回读环境变量 MINISQL_CONFIG；仍没有就使用内置默认值。
        }
        const auto config = loadConfig(configFile, options.overrides, environment);
        // 加载配置：文件 + 命令行覆盖 + 环境，优先级由 loadConfig 决定。
        if (options.printConfig) { output << config.toJson().dump(2) << '\n'; return 0; }
        // --print-config：打印最终生效的配置后退出，不写任何文件。
        if (options.checkConfig) { output << "Configuration valid\n"; return 0; }
        // --check-config：只做校验，打印结论后退出。
        if (options.execute) { throw unavailable("SQL execution"); }
        // -e/--execute：当前阶段还不能执行 SQL，按"未实现"报错。
        if (config.mode == "server") { throw unavailable("HTTP/WebSocket server"); }
        // server 模式尚未实现，同样按"未实现"报错，避免假装启动成功。

        prepareRuntimeDirectories(config);
        // 创建数据、WAL、日志等运行目录，保证后续写入不会因为目录缺失失败。
        LogManager logs(config.logging);
        // 按配置初始化日志管理器。
        logs.log("server", spdlog::level::info, "MiniSQL foundation starting", {"startup", "-"});
        // 记录启动日志。
        output << fmt::format("MiniSQL {} | mode=cli | SQL engine: not implemented\n", kVersion);
        // 打印启动横幅，明确当前 SQL 引擎尚未实现。
        output << "Type .help for commands; .quit to exit.\n";
        // 给用户一行最小提示。
        std::string line;
        // 交互式输入缓冲。
        while (output << "minisql> " << std::flush, std::getline(input, line)) {
        // 先打印提示符并立即刷新，再读一行；读不到（EOF）就结束循环。
            const auto command = trim(line);
            // 去掉首尾空白。
            if (command.empty()) { continue; }
            // 空行直接忽略，不报错。
            if (command == ".quit" || command == ".exit") { break; }
            // 退出命令：跳出循环，走正常关闭流程。
            if (command == ".help") { output << ".help  .version  .config  .quit  .exit\nSQL execution is not implemented yet.\n"; }
            // .help：列出当前可用的点命令。
            else if (command == ".version") { output << kVersion << '\n'; }
            // .version：打印版本号。
            else if (command == ".config") { output << config.toJson().dump(2) << '\n'; }
            // .config：打印当前生效的配置。
            else {
            // 其余输入一律当成 SQL，但当前阶段还没有 SQL 引擎。
                // Do not log raw SQL: it can contain passwords and user data.
                // 不要把原始 SQL 写进日志：它可能包含口令与用户数据。
                logs.log("sql", spdlog::level::warn, "Query rejected: SQL engine unavailable");
                // 只记录"查询被拒"这一事实与原因。
                error << unavailable("SQL execution").toJson().dump() << '\n';
                // 把结构化错误写到错误流，前端可以按 JSON 解析。
            }
        }
        if (input.bad() || (input.fail() && !input.eof())) {
        // 循环结束不是因为 EOF，而是输入流真的出错了。
            throw MiniSqlError(ErrorCode::InvalidArgument, "Failed to read CLI input");
            // 按参数/输入错误上报。
        }
        logs.log("server", spdlog::level::info, "MiniSQL foundation stopped", {"shutdown", "-"});
        // 记录关闭日志。
        logs.flush();
        // 显式刷盘，保证退出前日志已经落盘。
        return 0;
        // 正常退出。
    } catch (const MiniSqlError& exception) {
    // 已识别的错误：直接输出它的 JSON 表示。
        error << exception.toJson().dump() << '\n';
        // 写错误流，调用方（含测试）按 JSON 解析错误类型、位置与原因。
        return 1;
        // 返回失败退出码。
    } catch (const std::exception&) {
    // 未预料到的异常。
        // Avoid leaking implementation details or sensitive exception payloads.
        // 不要把实现细节或可能含敏感数据的异常内容泄露出去。
        error << MiniSqlError(ErrorCode::Internal, "Unexpected internal failure").toJson().dump() << '\n';
        // 统一改写成一条内部错误。
        return 1;
        // 同样返回失败退出码。
    }
}
// 入口函数结束。
} // namespace minisql
