#include "minisql/common/logger.hpp"
#include "minisql/common/error.hpp"
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace minisql {
namespace {
std::string singleLine(std::string_view value) {
// 把消息里的换行、回车与控制字符替换成可见形式，保证一条日志只占一行。
    std::string result;
    // 结果缓冲。
    for (const unsigned char c : value) {
    // 逐字符扫描；用 unsigned char 避免高字节被当作负数。
        if (c == '\n') { result += "\\n"; }
        // 换行转义成两个可见字符 \n。
        else if (c == '\r') { result += "\\r"; }
        // 回车转义成 \r。
        else if (c < 32 || c == 127) { result += '?'; }
        // 其他控制字符统一替换成问号，防止破坏日志文件结构。
        else { result += static_cast<char>(c); }
        // 普通字符原样保留。
    }
    return result;
    // 返回处理后的一行文本。
}

spdlog::level::level_enum logLevel(const std::string& name) {
// 把配置里的级别名转成 spdlog 的枚举。
    if (name == "TRACE") return spdlog::level::trace;
    // TRACE：最详细。
    if (name == "DEBUG") return spdlog::level::debug;
    // DEBUG：调试信息。
    if (name == "INFO") return spdlog::level::info;
    // INFO：常规信息。
    if (name == "WARN") return spdlog::level::warn;
    // WARN：警告。
    if (name == "ERROR") return spdlog::level::err;
    // ERROR：错误。
    if (name == "CRITICAL") return spdlog::level::critical;
    // CRITICAL：严重错误。
    if (name == "OFF") return spdlog::level::off;
    // OFF：关闭日志。
    throw MiniSqlError(ErrorCode::Configuration, "Unknown logging.level");
    // 其余取值都视为配置错误；正常情况下 Config::validate 已经挡过一遍。
}
} // namespace

LogManager::LogManager(const LogConfig& config) {
// 构造：创建目录并为每个模块建立日志器。
    Config validation;
    // 借用 Config 的校验逻辑，避免重复写一遍范围检查。
    validation.logging = config;
    // 把待构造的日志配置塞进去。
    validation.validate();
    // 级别、目录、轮转参数不合法会在这里抛错。
    try {
        const auto directory = pathFromUtf8(config.directory);
        // 把 UTF-8 路径转成文件系统路径，兼容中文目录。
        std::filesystem::create_directories(directory);
        // 目录不存在就创建。
        std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> console;
        // 控制台接收器，稍后按配置决定是否启用。
        if (config.console) { console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(); }
        // 配置要求控制台输出时才创建它。
        for (const auto* module : {"common", "server", "sql", "catalog", "execution", "optimizer", "storage", "transaction", "recovery"}) {
        // 九个模块各建一个日志器，因此日志会分成九个文件。
            const auto path = directory / (std::string(module) + ".log");
            // 拼出该模块的日志文件路径。
            // Native Windows wide paths support Chinese workspace/user names.
#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
            const auto filename = path.wstring();
            // 用宽字符路径，保证含中文的用户目录可写。
#else
            const auto filename = path.string();
            // 其他平台直接用窄字符路径。
#endif
            std::vector<spdlog::sink_ptr> sinks;
            // 该模块的接收器列表。
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filename, config.maxFileSize, config.maxFiles));
            // 第一个接收器是轮转文件：超过阈值就换新文件并保留指定份数。
            if (console) { sinks.push_back(console); }
            // 启用了控制台就把控制台接收器也挂上，实现一份日志两处输出。
            auto logger = std::make_shared<spdlog::logger>(module, sinks.begin(), sinks.end());
            // 用模块名与接收器列表创建日志器。
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [thread=%t] %v");
            // 设置行格式：时间、级别、模块名、线程号、正文。
            logger->set_level(logLevel(config.level));
            // 设置最低输出级别。
            logger->flush_on(spdlog::level::err);
            // 出现错误级别日志时立即刷盘，方便崩溃后排查。
            loggers_.emplace(module, std::move(logger));
            // 放进模块名到日志器的映射表。
        }
    } catch (const std::exception&) {
    // 文件建不出来、目录没权限等都会走到这里。
        throw MiniSqlError(ErrorCode::Storage, "Cannot initialize log files", {}, "Check log directory permissions and available disk space.");
        // 统一转成存储类错误，并给出排查方向。
    }
}

LogManager::~LogManager() {
// 析构：尽力把剩余日志刷出去。
    try { flush(); } catch (...) { /* Destructors cannot propagate flush failures. */ }
    // 析构函数不能抛异常，因此这里吞掉刷盘失败。
}

void LogManager::log(std::string_view module, spdlog::level::level_enum level,
                     std::string_view message, const LogContext& context) const {
// 写一条日志：按模块名找到对应日志器，再带上请求号与事务号。
    const auto it = loggers_.find(std::string(module));
    // 在映射表里查找模块。
    if (it == loggers_.end()) { throw MiniSqlError(ErrorCode::InvalidArgument, "Unknown log module"); }
    // 模块名写错属于调用方参数错误，直接抛异常暴露问题。
    it->second->log(level, "[request={}] [txn={}] {}",
                    singleLine(context.requestId), singleLine(context.transactionId), singleLine(message));
    // 三个文本都先做单行化处理，再按固定格式写入。
}

void LogManager::flush() const {
// 刷新所有模块的日志缓冲。
    for (const auto& [name, logger] : loggers_) { (void)name; logger->flush(); }
    // 只需要 logger，模块名用不上，(void)name 是显式忽略，避免未使用变量警告。
}
} // namespace minisql
