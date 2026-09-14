#include "minisql/common/error.hpp"
#include <nlohmann/json.hpp>

namespace minisql {
std::string errorType(ErrorCode code) {
// 错误码到字符串名的翻译函数，诊断输出与 HTTP 响应都依赖它。
    switch (code) {
    // 按错误码逐一分支。
    case ErrorCode::Ok: return "OK";
    // 0：成功。
    case ErrorCode::InvalidArgument: return "InvalidArgumentError";
    // 1001：调用参数不合法。
    case ErrorCode::Configuration: return "ConfigurationError";
    // 1002：配置文件或配置项有问题。
    case ErrorCode::Lexical: return "LexicalError";
    // 2001：词法分析阶段报错。
    case ErrorCode::Syntax: return "SyntaxError";
    // 2002：语法分析阶段报错。
    case ErrorCode::Semantic: return "SemanticError";
    // 2003：语义检查阶段报错。
    case ErrorCode::IntegerOutOfRange: return "SEM_INTEGER_OUT_OF_RANGE";
    case ErrorCode::PlanStaleSchema: return "PLAN_STALE_SCHEMA";
    case ErrorCode::Catalog: return "CatalogError";
    // 3001：目录层报错，例如表不存在。
    case ErrorCode::Storage: return "StorageError";
    // 4001：存储层报错，例如页损坏。
    case ErrorCode::Execution: return "ExecutionError";
    // 5001：执行阶段报错。
    case ErrorCode::Cancelled: return "CancelledError";
    // 5002：查询被用户取消。
    case ErrorCode::Transaction: return "TransactionError";
    // 6001：事务状态不合法。
    case ErrorCode::Permission: return "PermissionError";
    // 7001：权限校验未通过。
    case ErrorCode::Network: return "NetworkError";
    // 8001：网络层错误。
    case ErrorCode::NotImplemented: return "NotImplementedError";
    // 9001：功能尚未实现。
    default: return "InternalError";
    // 其余情况统一按内部错误兜底，避免出现没有名字的错误码。
    }
}

MiniSqlError::MiniSqlError(ErrorCode code, std::string message,
                         SourceLocation location, std::string suggestion,
                         std::string actual, std::vector<std::string> expected)
    : std::runtime_error(std::move(message)), code_(code), location_(location),
      suggestion_(std::move(suggestion)), actual_(std::move(actual)),
      expected_(std::move(expected)) {}
// 初始化列表：先用 message 初始化父类 runtime_error（what() 返回的就是它），
// 再把其余五个字段逐个赋值；函数体为空，所有工作都在初始化列表完成。

nlohmann::json MiniSqlError::toJson() const {
// 把错误对象转成统一 JSON，CLI 与 HTTP 的失败响应都用这个形状。
    return {{"success", false}, {"error", {
    // 最外层 success 恒为 false，真正的详情放在 error 子对象里。
        {"type", errorType(code_)}, {"code", static_cast<int>(code_)},
        // type 是可读名字，code 是数字编码；枚举需要显式转成 int。
        {"message", what()}, {"line", location_.line},
        // message 来自父类保存的文本，line 是出错行号。
        {"column", location_.column}, {"suggestion", suggestion_},
        // column 是出错列号，suggestion 是修复建议。
        {"actual", actual_}, {"expected", expected_}}}};
        // actual 是实际读到的符号，expected 是期望符号列表。
}

Status::Status(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {
    if (ok() && !message_.empty()) {
        // 校验：声称成功却又带了消息，属于自相矛盾，直接抛标准异常。
        throw std::invalid_argument("OK status cannot contain an error message");
    }
}
// 构造完成后，要么是成功且无消息，要么是失败且带消息。

void Status::throwIfError() const {
    if (!ok()) { throw MiniSqlError(code_, message_); }
    // 失败时把轻量 Status 升级成完整的 MiniSqlError；
    // 成功时什么也不做，调用方可以继续往下走。
}
} // namespace minisql
