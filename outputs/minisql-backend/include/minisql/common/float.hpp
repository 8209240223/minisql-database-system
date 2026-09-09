#pragma once
#include "minisql/common/error.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace minisql {
inline double requireFiniteFloat(double value, SourceLocation location = {}) {
    if (!std::isfinite(value))
        throw MiniSqlError(ErrorCode::Execution, "FLOAT value must be finite", location);
    return value;
}

inline double parseFiniteFloat(std::string_view text, ErrorCode code = ErrorCode::Execution,
                               SourceLocation location = {}) {
    if (text.empty())
        throw MiniSqlError(code, "FLOAT literal is empty", location);
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    if (*begin == '+') ++begin;
    if (begin == end)
        throw MiniSqlError(code, "Invalid FLOAT literal", location);
    double value{};
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
        throw MiniSqlError(code, "Invalid or non-finite FLOAT literal", location);
    return value;
}

inline std::string formatFiniteFloat(double value, SourceLocation location = {}) {
    requireFiniteFloat(value, location);
    char buffer[64]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{})
        throw MiniSqlError(ErrorCode::Internal, "FLOAT formatting failed", location);
    return std::string(buffer, converted.ptr);
}
}
