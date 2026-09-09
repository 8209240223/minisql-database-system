#pragma once

#include "minisql/common/config.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace minisql {
struct LogContext {
    std::string requestId{"-"};
    std::string transactionId{"-"};
};

// Construct once before worker threads start. Concurrent log()/flush() are safe.
// Separate instances must use separate log directories.
class LogManager {
public:
    explicit LogManager(const LogConfig& config);
    ~LogManager();
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;
    void log(std::string_view module, spdlog::level::level_enum level,
             std::string_view message, const LogContext& context = {}) const;
    void flush() const;
private:
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers_;
};
} // namespace minisql
