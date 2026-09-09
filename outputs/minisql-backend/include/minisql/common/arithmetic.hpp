#pragma once
#include "minisql/common/error.hpp"
#include <cstdint>
#include <limits>

namespace minisql {
inline bool isArithmetic(const std::string& op) { return op == "+" || op == "-" || op == "*" || op == "/"; }
inline std::int64_t arithmetic64(const std::string& op, std::int64_t left, std::int64_t right, SourceLocation location = {}) {
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    const auto overflow = [&]() { throw MiniSqlError(ErrorCode::Execution, "BIGINT arithmetic overflow", location); };
    if (op == "+") {
        if ((right > 0 && left > high - right) || (right < 0 && left < low - right)) overflow();
        return left + right;
    }
    if (op == "-") {
        if ((right > 0 && left < low + right) || (right < 0 && left > high + right)) overflow();
        return left - right;
    }
    if (op == "*") {
        // 在执行有符号乘法前按符号分区检查，避免先溢出后检查的未定义行为。
        if (left > 0) {
            if ((right > 0 && left > high / right) || (right < 0 && right < low / left)) overflow();
        } else if (left < 0) {
            if ((right > 0 && left < low / right) || (right < 0 && left < high / right)) overflow();
        }
        return left * right;
    }
    if (op == "/") {
        if (right == 0) throw MiniSqlError(ErrorCode::Execution, "Division by zero", location);
        if (left == low && right == -1) overflow();
        return left / right;
    }
    throw MiniSqlError(ErrorCode::Internal, "Unknown arithmetic operator", location);
}
inline std::int32_t arithmetic(const std::string& op, std::int32_t left, std::int32_t right, SourceLocation location = {}) {
    const auto result = arithmetic64(op, left, right, location);
    if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max())
        throw MiniSqlError(ErrorCode::Execution, "INT32 arithmetic overflow", location);
    return static_cast<std::int32_t>(result);
}
}
