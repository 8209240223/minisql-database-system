#include "minisql/common/logger.hpp"
#include "minisql/common/error.hpp"
#include <filesystem>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace minisql {
namespace {
std::string singleLine(std::string_view value) {
    std::string result;
    for (const unsigned char c : value) {
        if (c == '\n') { result += "\\n"; }
        else if (c == '\r') { result += "\\r"; }
        else if (c < 32 || c == 127) { result += '?'; }
        else { result += static_cast<char>(c); }
    }
    return result;
}

spdlog::level::level_enum logLevel(const std::string& name) {
    if (name == "TRACE") return spdlog::level::trace;
    if (name == "DEBUG") return spdlog::level::debug;
    if (name == "INFO") return spdlog::level::info;
    if (name == "WARN") return spdlog::level::warn;
    if (name == "ERROR") return spdlog::level::err;
    if (name == "CRITICAL") return spdlog::level::critical;
    if (name == "OFF") return spdlog::level::off;
    throw MiniSqlError(ErrorCode::Configuration, "Unknown logging.level");
}
} // namespace

LogManager::LogManager(const LogConfig& config) {
    Config validation;
    validation.logging = config;
    validation.validate();
    try {
        const auto directory = pathFromUtf8(config.directory);
        std::filesystem::create_directories(directory);
        std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> console;
        if (config.console) { console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>(); }
        for (const auto* module : {"common", "server", "sql", "catalog", "execution", "optimizer", "storage", "transaction", "recovery"}) {
            const auto path = directory / (std::string(module) + ".log");
            // Native Windows wide paths support Chinese workspace/user names.
#if defined(_WIN32) && defined(SPDLOG_WCHAR_FILENAMES)
            const auto filename = path.wstring();
#else
            const auto filename = path.string();
#endif
            std::vector<spdlog::sink_ptr> sinks;
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filename, config.maxFileSize, config.maxFiles));
            if (console) { sinks.push_back(console); }
            auto logger = std::make_shared<spdlog::logger>(module, sinks.begin(), sinks.end());
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [thread=%t] %v");
            logger->set_level(logLevel(config.level));
            logger->flush_on(spdlog::level::err);
            loggers_.emplace(module, std::move(logger));
        }
    } catch (const std::exception&) {
        throw MiniSqlError(ErrorCode::Storage, "Cannot initialize log files", {}, "Check log directory permissions and available disk space.");
    }
}

LogManager::~LogManager() {
    try { flush(); } catch (...) { /* Destructors cannot propagate flush failures. */ }
}

void LogManager::log(std::string_view module, spdlog::level::level_enum level,
                     std::string_view message, const LogContext& context) const {
    const auto it = loggers_.find(std::string(module));
    if (it == loggers_.end()) { throw MiniSqlError(ErrorCode::InvalidArgument, "Unknown log module"); }
    it->second->log(level, "[request={}] [txn={}] {}",
                    singleLine(context.requestId), singleLine(context.transactionId), singleLine(message));
}

void LogManager::flush() const {
    for (const auto& [name, logger] : loggers_) { (void)name; logger->flush(); }
}
} // namespace minisql
