#pragma once

#include "minisql/common/config.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <spdlog/spdlog.h>

namespace minisql {
struct LogContext {
// 每条日志附带的上下文信息。
    std::string requestId{"-"};
    // 请求编号，默认是短横线，表示没有上下文。
    std::string transactionId{"-"};
    // 事务编号，同样默认短横线。
};

// Construct once before worker threads start. Concurrent log()/flush() are safe.
// Separate instances must use separate log directories.
class LogManager {
// 日志管理器：按模块名分发到不同日志文件。
public:
    explicit LogManager(const LogConfig& config);
    // 构造时创建目录与各模块的日志器；explicit 禁止隐式转换。
    ~LogManager();
    // 析构时尽力刷盘，保证缓冲区里的日志不丢。
    LogManager(const LogManager&) = delete;
    // 禁止拷贝：日志器内部持有文件句柄，复制会产生两份状态。
    LogManager& operator=(const LogManager&) = delete;
    // 同样禁止拷贝赋值。
    void log(std::string_view module, spdlog::level::level_enum level,
             std::string_view message, const LogContext& context = {}) const;
    // 写一条日志：指定模块名、级别、消息与上下文。
    void flush() const;
    // 把所有模块的日志刷到磁盘。
private:
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> loggers_;
    // 模块名到日志器对象的映射表。
};
} // namespace minisql
