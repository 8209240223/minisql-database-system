#pragma once
#include "minisql/common/error.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace minisql {
inline std::int32_t parseIsoDate(std::string_view text, SourceLocation location = {}, ErrorCode code = ErrorCode::Execution) {
    const auto invalid = [&]() -> void { throw MiniSqlError(code, "DATE requires a valid YYYY-MM-DD in years 0001 through 9999", location); };
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') invalid();
    const auto number = [&](std::size_t start, std::size_t length) {
        unsigned value = 0;
        for (std::size_t i=start;i<start+length;++i) {
            if (text[i] < '0' || text[i] > '9') invalid();
            value = value * 10 + static_cast<unsigned>(text[i]-'0');
        }
        return value;
    };
    const auto year=number(0,4), month=number(5,2), day=number(8,2);
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month}, std::chrono::day{day}};
    if (year == 0 || !date.ok()) invalid();
    return static_cast<std::int32_t>(std::chrono::sys_days{date}.time_since_epoch().count());
}
inline std::string formatIsoDate(std::int32_t days, ErrorCode code = ErrorCode::Storage) {
    using namespace std::chrono;
    constexpr auto first = sys_days{year{1}/1/1}.time_since_epoch().count();
    constexpr auto last = sys_days{year{9999}/12/31}.time_since_epoch().count();
    if (days < first || days > last) throw MiniSqlError(code, "DATE day number outside supported calendar range");
    const year_month_day date{sys_days{std::chrono::days{days}}};
    char text[11];
    std::snprintf(text,sizeof(text),"%04d-%02u-%02u",static_cast<int>(date.year()),static_cast<unsigned>(date.month()),static_cast<unsigned>(date.day()));
    return text;
}
inline std::optional<std::string> dateLiteralText(std::string_view raw) {
    if (raw.size() < 6 || (raw[0]!='D' && raw[0]!='d') || (raw[1]!='A' && raw[1]!='a') ||
        (raw[2]!='T' && raw[2]!='t') || (raw[3]!='E' && raw[3]!='e') || raw[4]!='\'' || raw.back()!='\'') return std::nullopt;
    return std::string(raw.substr(5,raw.size()-6));
}
}
