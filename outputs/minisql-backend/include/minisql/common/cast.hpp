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
    if (value.is_null()) return nullptr;
    if (target == "date") {
        if (!value.is_string()) throw MiniSqlError(ErrorCode::Execution,"DATE CAST requires ISO date text",location);
        return formatIsoDate(parseIsoDate(value.get_ref<const std::string&>(),location));
    }
    if (target == "bool") {
        if (value.is_boolean()) return value;
        if (value.is_string()) {
            auto text = value.get<std::string>();
            for (char& c : text) if (c >= 'a' && c <= 'z') c -= 32;
            if (text == "TRUE") return true;
            if (text == "FALSE") return false;
        }
        throw MiniSqlError(ErrorCode::Execution, "BOOL CAST requires complete TRUE or FALSE text", location);
    }
    if (target == "float") {
        if (value.is_number_float()) return requireFiniteFloat(value.get<double>(), location);
        if (value.is_number_integer()) return requireFiniteFloat(static_cast<double>(value.get<std::int64_t>()), location);
        if (value.is_string()) return parseFiniteFloat(value.get_ref<const std::string&>(), ErrorCode::Execution, location);
        throw MiniSqlError(ErrorCode::Execution, "FLOAT CAST requires a numeric value or complete numeric text", location);
    }
    if (const auto decimal = decimalType(target)) {
        try {
            if (value.is_string()) return ExactDecimal::parse(value.get_ref<const std::string&>(), decimal->precision, decimal->scale, true).format();
            if (value.is_number_integer()) return ExactDecimal::parse(std::to_string(value.get<std::int64_t>()), decimal->precision, decimal->scale, true).format();
            if (value.is_number_float()) return ExactDecimal::parse(formatFiniteFloat(value.get<double>(), location), decimal->precision, decimal->scale, true).format();
        } catch (const MiniSqlError& error) {
            throw MiniSqlError(error.code(), error.what(), location);
        }
        throw MiniSqlError(ErrorCode::Execution, "Unsupported DECIMAL CAST source", location);
    }
    if (stringType(target)) {
        std::string text;
        if (value.is_boolean()) text=value.get<bool>() ? "TRUE" : "FALSE";
        else if (value.is_string()) text=value.get<std::string>();
        else if (value.is_number_integer()) text=std::to_string(value.get<std::int64_t>());
        else if (value.is_number_float()) text=formatFiniteFloat(value.get<double>(), location);
        else throw MiniSqlError(ErrorCode::Execution,"Unsupported VARCHAR CAST source",location);
        if (const auto limit=varcharLength(target)) validateVarchar(text,*limit,location);
        return text;
    } else if (target == "int" || target == "bigint") {
        std::int64_t integer{};
        if (value.is_string()) {
            const auto& text = value.get_ref<const std::string&>();
            const auto* begin = text.data();
            const auto* end = begin + text.size();
            // from_chars 不接受正号；移除一个正号后仍必须检查整个字符串。
            if (begin != end && *begin == '+') {
                ++begin;
                if (begin != end && (*begin == '+' || *begin == '-'))
                    throw MiniSqlError(ErrorCode::Execution, "Invalid integer CAST", location);
            }
            const auto parsed = std::from_chars(begin, end, integer);
            if (parsed.ec != std::errc{} || parsed.ptr != end)
                throw MiniSqlError(ErrorCode::Execution, "Invalid or out-of-range integer CAST", location);
        } else if (value.is_number_integer()) integer = value.get<std::int64_t>();
        else if (value.is_number_float()) {
            const auto number = requireFiniteFloat(value.get<double>(), location);
            if (std::trunc(number) != number || number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                number >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                throw MiniSqlError(ErrorCode::Execution, "FLOAT CAST to integer requires an in-range integral value", location);
            integer = static_cast<std::int64_t>(number);
        }
        else throw MiniSqlError(ErrorCode::Execution, "Unsupported CAST source", location);
        if (target == "int" && (integer < INT32_MIN || integer > INT32_MAX))
            throw MiniSqlError(ErrorCode::Execution, "CAST result outside INT32 range", location);
        return integer;
    }
    throw MiniSqlError(ErrorCode::Execution, "Unsupported CAST conversion", location);
}
}
