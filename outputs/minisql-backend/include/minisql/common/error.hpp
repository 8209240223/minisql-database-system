#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <nlohmann/json_fwd.hpp>

namespace minisql {

enum class ErrorCode {
    Ok = 0, InvalidArgument = 1001, Configuration = 1002,
    Lexical = 2001, Syntax = 2002, Semantic = 2003,
    Catalog = 3001, Storage = 4001, Execution = 5001, Cancelled = 5002,
    Transaction = 6001, Permission = 7001, Network = 8001,
    NotImplemented = 9001, Internal = 9999
};

struct SourceLocation {
    std::size_t line{0};   // 1-based; 0 means location unavailable.
    std::size_t column{0};
};

std::string errorType(ErrorCode code);

class MiniSqlError : public std::runtime_error {
public:
    MiniSqlError(ErrorCode code, std::string message,
                 SourceLocation location = {}, std::string suggestion = {});
    ErrorCode code() const noexcept { return code_; }
    SourceLocation location() const noexcept { return location_; }
    const std::string& suggestion() const noexcept { return suggestion_; }
    nlohmann::json toJson() const;
private:
    ErrorCode code_;
    SourceLocation location_;
    std::string suggestion_;
};

class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string message);
    bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    void throwIfError() const;
private:
    ErrorCode code_{ErrorCode::Ok};
    std::string message_;
};

template <typename T>
class Result {
public:
    explicit Result(T value) : state_(std::move(value)) {}
    explicit Result(Status error) : state_(std::move(error)) {
        if (std::get<Status>(state_).ok()) {
            throw std::invalid_argument("An error Result cannot contain OK status");
        }
    }
    bool ok() const noexcept { return std::holds_alternative<T>(state_); }
    const T& value() const {
        if (!ok()) { std::get<Status>(state_).throwIfError(); }
        return std::get<T>(state_);
    }
    T& value() {
        if (!ok()) { std::get<Status>(state_).throwIfError(); }
        return std::get<T>(state_);
    }
    Status status() const { return ok() ? Status{} : std::get<Status>(state_); }
private:
    std::variant<T, Status> state_;
};

} // namespace minisql
