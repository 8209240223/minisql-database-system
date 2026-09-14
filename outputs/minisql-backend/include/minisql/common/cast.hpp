#pragma once
#include "minisql/common/error.hpp"
#include "minisql/common/decimal.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/float.hpp"
#include "minisql/common/varchar.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace minisql {
inline nlohmann::json castValue(const nlohmann::json& value, const std::string& target, SourceLocation location) {
// CAST 的实现：把 value 转成 target 描述的类型，失败即抛错。
    if (value.is_null()) return nullptr;
    // 空值转任何类型仍然是空值，这是 SQL 的三值逻辑要求。
    if (target == "date") {
    // 目标为日期。
        if (!value.is_string()) throw MiniSqlError(ErrorCode::Execution,"DATE CAST requires ISO date text",location);
        // 只有文本才能解析成日期。
        return formatIsoDate(parseIsoDate(value.get_ref<const std::string&>(),location));
        // 先解析校验，再格式化回标准文本，等价于一次规范化。
    }
    if (target == "bool") {
    // 目标为布尔。
        if (value.is_boolean()) return value;
        // 本身已是布尔就直接返回。
        if (value.is_string()) {
        // 文本形式需要识别 TRUE 与 FALSE。
            auto text = value.get<std::string>();
            // 取出文本副本以便原地转大写。
            for (char& c : text) if (c >= 'a' && c <= 'z') c -= 32;
            // 手工把小写字母转成大写，不依赖本地化设置。
            if (text == "TRUE") return true;
            // 识别真。
            if (text == "FALSE") return false;
            // 识别假。
        }
        throw MiniSqlError(ErrorCode::Execution, "BOOL CAST requires complete TRUE or FALSE text", location);
        // 其他写法一律拒绝，避免把部分匹配当成有效。
    }
    if (target == "float") {
    // 目标为浮点。
        if (value.is_number_float()) return requireFiniteFloat(value.get<double>(), location);
        // 已经是浮点，只需确认有限。
        if (value.is_number_integer()) return requireFiniteFloat(static_cast<double>(value.get<std::int64_t>()), location);
        // 整数可以安全提升为浮点。
        if (value.is_string()) return parseFiniteFloat(value.get_ref<const std::string&>(), ErrorCode::Execution, location);
        // 文本按浮点字面量解析并校验。
        throw MiniSqlError(ErrorCode::Execution, "FLOAT CAST requires a numeric value or complete numeric text", location);
        // 其余类型不支持。
    }
    if (const auto decimal = decimalType(target)) {
    // 目标为 DECIMAL(p,s)：先判断类型名是否合法。
        try {
            if (value.is_string()) return ExactDecimal::parse(value.get_ref<const std::string&>(), decimal->precision, decimal->scale, true).format();
            // 文本按目标精度标度解析，允许舍入后输出规范格式。
            if (value.is_number_integer()) return ExactDecimal::parse(std::to_string(value.get<std::int64_t>()), decimal->precision, decimal->scale, true).format();
            // 整数先转文本再走同一条解析路径，保证舍入规则一致。
            if (value.is_number_float()) return ExactDecimal::parse(formatFiniteFloat(value.get<double>(), location), decimal->precision, decimal->scale, true).format();
            // 浮点先格式化成最短精确文本，再解析，避免二进制误差被放大。
        } catch (const MiniSqlError& error) {
        // 解析过程中抛出的错误在这里换成 CAST 的位置信息。
            throw MiniSqlError(error.code(), error.what(), location);
            // 保留原始错误码与消息，只替换位置。
        }
        throw MiniSqlError(ErrorCode::Execution, "Unsupported DECIMAL CAST source", location);
        // 能走到这里说明源类型不在支持列表中。
    }
    if (stringType(target)) {
    // 目标为字符串类型（varchar 或 varchar(n)）。
        std::string text;
        // 转换后的文本。
        if (value.is_boolean()) text=value.get<bool>() ? "TRUE" : "FALSE";
        // 布尔转成大写文本。
        else if (value.is_string()) text=value.get<std::string>();
        // 字符串原样使用。
        else if (value.is_number_integer()) text=std::to_string(value.get<std::int64_t>());
        // 整数转十进制文本。
        else if (value.is_number_float()) text=formatFiniteFloat(value.get<double>(), location);
        // 浮点用统一格式输出。
        else throw MiniSqlError(ErrorCode::Execution,"Unsupported VARCHAR CAST source",location);
        // 其余类型不支持。
        if (const auto limit=varcharLength(target)) validateVarchar(text,*limit,location);
        // 如果目标带长度限制，按字符数校验，超出即报错。
        return text;
        // 返回转换结果。
    } else if (target == "int" || target == "bigint") {
    // 目标为整数类型。
        std::int64_t integer{};
        // 先统一按 64 位处理，最后再判断是否超出 int32。
        if (value.is_string()) {
        // 文本形式需要手工解析。
            const auto& text = value.get_ref<const std::string&>();
            // 取文本引用，避免复制。
            const auto* begin = text.data();
            // 起始指针。
            const auto* end = begin + text.size();
            // 结束指针。
            // from_chars 不接受正号；移除一个正号后仍必须检查整个字符串。
            if (begin != end && *begin == '+') {
            // 文本以正号开头。
                ++begin;
                // 跳过正号。
                if (begin != end && (*begin == '+' || *begin == '-'))
                    throw MiniSqlError(ErrorCode::Execution, "Invalid integer CAST", location);
                    // 正号后面又出现正负号，属于非法写法。
            }
            const auto parsed = std::from_chars(begin, end, integer);
            // 解析剩余部分。
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                throw MiniSqlError(ErrorCode::Execution, "Invalid or out-of-range integer CAST", location);
                // 解析失败或没消费完都算非法。
        } else if (value.is_number_integer()) integer = value.get<std::int64_t>();
        // 本身是整数直接取值。
        else if (value.is_number_float()) {
        // 浮点转整数需要额外约束。
            const auto number = requireFiniteFloat(value.get<double>(), location);
            // 先确认是有限值。
            if (std::trunc(number) != number || number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                number >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                throw MiniSqlError(ErrorCode::Execution, "FLOAT CAST to integer requires an in-range integral value", location);
                // 有小数部分或者超出范围都拒绝，避免静默截断。
            integer = static_cast<std::int64_t>(number);
            // 转换并保存。
        }
        else throw MiniSqlError(ErrorCode::Execution, "Unsupported CAST source", location);
        // 其余源类型不支持。
        if (target == "int" && (integer < INT32_MIN || integer > INT32_MAX))
            throw MiniSqlError(ErrorCode::Execution, "CAST result outside INT32 range", location);
            // 目标是 int 时还要检查 32 位范围。
        return integer;
        // 返回整数结果。
    }
    throw MiniSqlError(ErrorCode::Execution, "Unsupported CAST conversion", location);
    // 目标类型不认识，属于不支持的转换。
}
} // namespace minisql
