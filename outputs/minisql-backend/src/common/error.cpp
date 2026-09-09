#include "minisql/common/error.hpp"
#include <nlohmann/json.hpp>

namespace minisql {
std::string errorType(ErrorCode code) {
    switch (code) {
    case ErrorCode::Ok: return "OK";
    case ErrorCode::InvalidArgument: return "InvalidArgumentError";
    case ErrorCode::Configuration: return "ConfigurationError";
    case ErrorCode::Lexical: return "LexicalError";
    case ErrorCode::Syntax: return "SyntaxError";
    case ErrorCode::Semantic: return "SemanticError";
    case ErrorCode::Catalog: return "CatalogError";
    case ErrorCode::Storage: return "StorageError";
    case ErrorCode::Execution: return "ExecutionError";
    case ErrorCode::Cancelled: return "CancelledError";
    case ErrorCode::Transaction: return "TransactionError";
    case ErrorCode::Permission: return "PermissionError";
    case ErrorCode::Network: return "NetworkError";
    case ErrorCode::NotImplemented: return "NotImplementedError";
    default: return "InternalError";
    }
}

MiniSqlError::MiniSqlError(ErrorCode code, std::string message,
                         SourceLocation location, std::string suggestion)
    : std::runtime_error(std::move(message)), code_(code), location_(location),
      suggestion_(std::move(suggestion)) {}

nlohmann::json MiniSqlError::toJson() const {
    return {{"success", false}, {"error", {
        {"type", errorType(code_)}, {"code", static_cast<int>(code_)},
        {"message", what()}, {"line", location_.line},
        {"column", location_.column}, {"suggestion", suggestion_}}}};
}

Status::Status(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {
    if (ok() && !message_.empty()) {
        throw std::invalid_argument("OK status cannot contain an error message");
    }
}

void Status::throwIfError() const {
    if (!ok()) { throw MiniSqlError(code_, message_); }
}
} // namespace minisql
