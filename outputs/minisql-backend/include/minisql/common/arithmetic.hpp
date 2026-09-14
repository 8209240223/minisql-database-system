#pragma once
#include "minisql/common/error.hpp"
#include <cstdint>
#include <limits>

namespace minisql {
inline bool isArithmetic(const std::string& op) { return op == "+" || op == "-" || op == "*" || op == "/"; }
// 判断一个运算符是不是四则运算之一，计划与类型检查都靠它分流。
inline std::int64_t arithmetic64(const std::string& op, std::int64_t left, std::int64_t right, SourceLocation location = {}) {
// 在 64 位整数上做四则运算，每一步都先检查溢出再计算。
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    // int64 的最小值，用于减法与除法的边界判断。
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    // int64 的最大值。
    const auto overflow = [&]() { throw MiniSqlError(ErrorCode::Execution, "BIGINT arithmetic overflow", location); };
    // 溢出统一出口：带上出错位置，便于定位到具体表达式。
    if (op == "+") {
    // 加法分支。
        if ((right > 0 && left > high - right) || (right < 0 && left < low - right)) overflow();
        // 加正数可能上溢、加负数可能下溢，两种方向分别判断。
        return left + right;
        // 通过检查后才真正相加。
    }
    if (op == "-") {
    // 减法分支。
        if ((right > 0 && left < low + right) || (right < 0 && left > high + right)) overflow();
        // 减正数可能下溢、减负数可能上溢。
        return left - right;
        // 相减并返回。
    }
    if (op == "*") {
    // 乘法分支。
        // 在执行有符号乘法前按符号分区检查，避免先溢出后检查的未定义行为。
        if (left > 0) {
        // 左操作数为正。
            if ((right > 0 && left > high / right) || (right < 0 && right < low / left)) overflow();
            // 正乘正看是否超过上界，正乘负看是否低于下界。
        } else if (left < 0) {
        // 左操作数为负。
            if ((right > 0 && left < low / right) || (right < 0 && left < high / right)) overflow();
            // 负乘正与负乘负分别比较。
        }
        return left * right;
        // 检查通过后相乘。
    }
    if (op == "/") {
    // 除法分支。
        if (right == 0) throw MiniSqlError(ErrorCode::Execution, "Division by zero", location);
        // 除以零直接报错。
        if (left == low && right == -1) overflow();
        // 最小值除以负一的结果超出 int64 表示范围，属于溢出。
        return left / right;
        // 正常相除。
    }
    throw MiniSqlError(ErrorCode::Internal, "Unknown arithmetic operator", location);
    // 走到这里说明运算符不认识，属于内部错误。
}
inline std::int32_t arithmetic(const std::string& op, std::int32_t left, std::int32_t right, SourceLocation location = {}) {
// 32 位版本：先按 64 位算，再判断结果能否放回 int32。
    const auto result = arithmetic64(op, left, right, location);
    // 借用 64 位实现，避免在 32 位上做复杂边界判断。
    if (result < std::numeric_limits<std::int32_t>::min() || result > std::numeric_limits<std::int32_t>::max())
    // 结果超出 int32 范围。
        throw MiniSqlError(ErrorCode::Execution, "INT32 arithmetic overflow", location);
        // 按 INT 溢出报错。
    return static_cast<std::int32_t>(result);
    // 安全地收窄返回。
}
} // namespace minisql
