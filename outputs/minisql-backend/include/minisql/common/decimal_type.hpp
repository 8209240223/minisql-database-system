#pragma once
#include "minisql/common/error.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>

namespace minisql {
struct DecimalType {
    unsigned precision;
    unsigned scale;
    std::string name() const { return "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")"; }
};
struct DecimalLiteral { DecimalType type; std::string value; };
inline DecimalLiteral decimalLiteral(std::string_view raw, SourceLocation location = {}) {
    const auto invalid = [&]() { throw MiniSqlError(ErrorCode::Semantic, "Invalid or out-of-range DECIMAL literal", location); };
    const bool negative = raw.starts_with('-');
    if (raw.starts_with('-') || raw.starts_with('+')) raw.remove_prefix(1);
    const auto dot = raw.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == raw.size()) invalid();
    for (std::size_t i = 0; i < raw.size(); ++i) if (i != dot && (raw[i] < '0' || raw[i] > '9')) invalid();
    const auto scale = raw.size() - dot - 1;
    std::size_t first = 0;
    while (first < dot && raw[first] == '0') ++first;
    const auto precision = dot - first + scale;
    if (scale > 38 || precision > 38) invalid();
    std::string value = first == dot ? "0" : std::string(raw.substr(first, dot - first));
    value += raw.substr(dot);
    if (negative && raw.find_first_not_of("0.") != std::string_view::npos) value.insert(0, 1, '-');
    return {{static_cast<unsigned>(precision), static_cast<unsigned>(scale)}, std::move(value)};
}
inline std::optional<DecimalType> decimalType(std::string_view type) {
    if (!type.starts_with("decimal(") || !type.ends_with(')')) return std::nullopt;
    type.remove_prefix(8);type.remove_suffix(1);
    const auto comma = type.find(',');
    if (comma == std::string_view::npos) return std::nullopt;
    DecimalType result{};
    const auto p = std::from_chars(type.data(), type.data() + comma, result.precision);
    const auto s = std::from_chars(type.data() + comma + 1, type.data() + type.size(), result.scale);
    if (p.ec != std::errc{} || p.ptr != type.data() + comma || s.ec != std::errc{} || s.ptr != type.data() + type.size() ||
        result.precision == 0 || result.precision > 38 || result.scale > result.precision) return std::nullopt;
    return result;
}
inline DecimalType decimalArithmeticType(std::string_view op, unsigned leftScale, unsigned rightScale, SourceLocation location = {}) {
    unsigned scale = std::max(leftScale, rightScale);
    if (op == "*") scale = leftScale + rightScale;
    else if (op == "/") scale = std::max(scale, 6u);
    else if (op != "+" && op != "-") throw MiniSqlError(ErrorCode::Internal, "Unknown DECIMAL arithmetic operator", location);
    if (leftScale > 38 || rightScale > 38 || scale > 38)
        throw MiniSqlError(ErrorCode::Semantic, "DECIMAL arithmetic scale exceeds precision 38", location);
    // 表达式结果采用 38 位容量，不裁剪小数位；实际系数溢出在求值时检查。
    return {38, scale};
}
}
