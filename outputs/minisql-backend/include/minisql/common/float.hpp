#pragma once
#include "minisql/common/error.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace minisql {
inline double requireFiniteFloat(double value, SourceLocation location = {}) {
// 校验浮点必须是有限值：拒绝无穷大与 NaN。
    if (!std::isfinite(value))
    // isfinite 为假说明是 inf 或 nan。
        throw MiniSqlError(ErrorCode::Execution, "FLOAT value must be finite", location);
        // 这类值无法参与正常比较与聚合，直接按执行错误抛出。
    return value;
    // 通过校验后原样返回，便于链式调用。
}

inline double parseFiniteFloat(std::string_view text, ErrorCode code = ErrorCode::Execution,
                               SourceLocation location = {}) {
// 把文本解析成浮点数，并确保结果是有限值。
    if (text.empty())
    // 空字符串没有意义。
        throw MiniSqlError(code, "FLOAT literal is empty", location);
        // 按调用方指定的错误码抛出。
    const auto* begin = text.data();
    // 指向文本首字符。
    const auto* end = begin + text.size();
    // 指向文本末尾之后的位置。
    if (*begin == '+') ++begin;
    // 标准库不接受前导正号，这里手工跳过。
    if (begin == end)
    // 只有一个正号，跳过后就空了。
        throw MiniSqlError(code, "Invalid FLOAT literal", location);
        // 判为非法字面量。
    double value{};
    const auto parsed = std::from_chars(begin, end, value, std::chars_format::general);
    // general 表示自动接受定点与科学计数法两种写法。
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value))
    // 三个条件任一成立都算失败：解析出错、没消费完整个字符串、结果不是有限数。
        throw MiniSqlError(code, "Invalid or non-finite FLOAT literal", location);
        // 统一报错。
    return value;
    // 返回解析结果。
}

inline std::string formatFiniteFloat(double value, SourceLocation location = {}) {
// 把浮点格式化成文本输出。
    requireFiniteFloat(value, location);
    // 先确认是有限值，否则格式化出来的文本无法再解析回去。
    char buffer[64]{};
    // 固定缓冲，64 字节足够容纳双精度最长的表示。
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    // max_digits10 保证“格式化再解析”能还原出完全相同的值。
    if (converted.ec != std::errc{})
    // 转换失败说明缓冲不足或实现不支持。
        throw MiniSqlError(ErrorCode::Internal, "FLOAT formatting failed", location);
        // 这属于内部问题，用兜底错误码。
    return std::string(buffer, converted.ptr);
    // 用转换后的结束指针确定长度，构造字符串返回。
}
} // namespace minisql
