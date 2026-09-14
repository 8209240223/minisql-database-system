#pragma once
#include "minisql/common/error.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <string_view>

namespace minisql {
struct DecimalType {
// DECIMAL 的类型描述：总位数与小数位数。
    unsigned precision;
    // 精度：整数位加小数位的总位数上限，本项目最大 38。
    unsigned scale;
    // 标度：小数点后保留多少位。
    std::string name() const { return "decimal(" + std::to_string(precision) + "," + std::to_string(scale) + ")"; }
    // 还原成标准类型名，用于目录展示与诊断信息。
};
struct DecimalLiteral { DecimalType type; std::string value; };
// 解析一个字面量得到的两个结果：推断出的类型，以及规范化后的数值文本。
inline DecimalLiteral decimalLiteral(std::string_view raw, SourceLocation location = {}) {
// 解析形如 3.14、-0.5 的 DECIMAL 字面量，并推断出它的精度与标度。
    const auto invalid = [&]() { throw MiniSqlError(ErrorCode::Semantic, "Invalid or out-of-range DECIMAL literal", location); };
    // 统一失败出口，属于语义错误。
    const bool negative = raw.starts_with('-');
    // 先记住符号，后面要把负号加回规范化文本。
    if (raw.starts_with('-') || raw.starts_with('+')) raw.remove_prefix(1);
    // 去掉可能的正负号，便于后续逐字符检查。
    const auto dot = raw.find('.');
    // 找小数点的位置。
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == raw.size()) invalid();
    // 必须有小数点，且小数点两侧都不能为空。
    for (std::size_t i = 0; i < raw.size(); ++i) if (i != dot && (raw[i] < '0' || raw[i] > '9')) invalid();
    // 除小数点外，所有字符都必须是数字。
    const auto scale = raw.size() - dot - 1;
    // 小数点之后的位数就是标度。
    std::size_t first = 0;
    // 用于跳过整数部分的前导零。
    while (first < dot && raw[first] == '0') ++first;
    // 前导零不计入精度。
    const auto precision = dot - first + scale;
    // 有效整数位加小数位就是精度。
    if (scale > 38 || precision > 38) invalid();
    // 超过 38 位就无法精确表示。
    std::string value = first == dot ? "0" : std::string(raw.substr(first, dot - first));
    // 去掉前导零后的整数部分；如果全是零就写一个 0。
    value += raw.substr(dot);
    // 接上小数点与小数部分。
    if (negative && raw.find_first_not_of("0.") != std::string_view::npos) value.insert(0, 1, '-');
    // 只有数值确实不为零时才保留负号，避免出现负零。
    return {{static_cast<unsigned>(precision), static_cast<unsigned>(scale)}, std::move(value)};
    // 返回类型描述与规范化文本。
}
inline std::optional<DecimalType> decimalType(std::string_view type) {
// 从类型名里解析 decimal(p,s)，失败时返回空。
    if (!type.starts_with("decimal(") || !type.ends_with(')')) return std::nullopt;
    // 必须形如 decimal(...)。
    type.remove_prefix(8);type.remove_suffix(1);
    // 去掉前缀 decimal( 共八个字符，再去掉结尾右括号。
    const auto comma = type.find(',');
    // 找分隔精度与标度的逗号。
    if (comma == std::string_view::npos) return std::nullopt;
    // 没有逗号就不是合法写法。
    DecimalType result{};
    const auto p = std::from_chars(type.data(), type.data() + comma, result.precision);
    // 解析逗号之前的精度。
    const auto s = std::from_chars(type.data() + comma + 1, type.data() + type.size(), result.scale);
    // 解析逗号之后的标度。
    if (p.ec != std::errc{} || p.ptr != type.data() + comma || s.ec != std::errc{} || s.ptr != type.data() + type.size() ||
        result.precision == 0 || result.precision > 38 || result.scale > result.precision) return std::nullopt;
    // 两段都必须是完整数字，且精度在 1 到 38 之间、标度不能超过精度。
    return result;
    // 返回解析后的类型。
}
inline DecimalType decimalArithmeticType(std::string_view op, unsigned leftScale, unsigned rightScale, SourceLocation location = {}) {
// 推导 DECIMAL 运算结果的标度：加法取较大小数位，乘法相加，除法至少保留六位。
    unsigned scale = std::max(leftScale, rightScale);
    // 加法与减法的默认规则：取两侧较大的标度。
    if (op == "*") scale = leftScale + rightScale;
    // 乘法的结果标度是两侧标度之和。
    else if (op == "/") scale = std::max(scale, 6u);
    // 除法的结果至少保留六位小数，避免过早截断。
    else if (op != "+" && op != "-") throw MiniSqlError(ErrorCode::Internal, "Unknown DECIMAL arithmetic operator", location);
    // 出现不认识的运算符说明是内部问题。
    if (leftScale > 38 || rightScale > 38 || scale > 38)
    // 标度超出上限就无法精确表示。
        throw MiniSqlError(ErrorCode::Semantic, "DECIMAL arithmetic scale exceeds precision 38", location);
        // 按语义错误拒绝。
    // 表达式结果统一采用 38 位容量，不裁剪小数位；实际系数溢出在求值时检查。
    return {38, scale};
    // 精度固定给 38，标度用上面推导出的值。
}
} // namespace minisql
