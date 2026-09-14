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
// 把 YYYY-MM-DD 文本解析成天数；inline 允许实现直接放在头文件里。
    const auto invalid = [&]() -> void { throw MiniSqlError(code, "DATE requires a valid YYYY-MM-DD in years 0001 through 9999", location); };
    // 统一失败出口：错误码可按调用场景指定，位置用于定位。
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') invalid();
    // 先做形状检查必须正好十位，第 5 位与第 8 位是短横线。
    const auto number = [&](std::size_t start, std::size_t length) {
    // 内部小工具：把指定位置的若干位数字解析成整数。
        unsigned value = 0;
        // 累加结果。
        for (std::size_t i=start;i<start+length;++i) {
        // 逐位扫描。
            if (text[i] < '0' || text[i] > '9') invalid();
            // 只要有一位不是数字就判非法。
            value = value * 10 + static_cast<unsigned>(text[i]-'0');
            // 按十进制累加。
        }
        return value;
        // 返回解析出的数字。
    };
    const auto year=number(0,4), month=number(5,2), day=number(8,2);
    // 依次取出年四位、月两位、日两位。
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(year)}, std::chrono::month{month}, std::chrono::day{day}};
    // 交给标准库组装成日期对象。
    if (year == 0 || !date.ok()) invalid();
    // year 为 0 或者标准库判定日期不存在（例如 2 月 30 日）都报错。
    return static_cast<std::int32_t>(std::chrono::sys_days{date}.time_since_epoch().count());
    // 转换成距离 1970-01-01 的天数并返回，这是存储与比较用的形式。
}
inline std::string formatIsoDate(std::int32_t days, ErrorCode code = ErrorCode::Storage) {
// 把天数还原成 YYYY-MM-DD 文本，用于查询结果输出。
    using namespace std::chrono;
    // 简化下面的书写。
    constexpr auto first = sys_days{year{1}/1/1}.time_since_epoch().count();
    // 支持下限：公元 1 年 1 月 1 日。
    constexpr auto last = sys_days{year{9999}/12/31}.time_since_epoch().count();
    // 支持上限：公元 9999 年 12 月 31 日。
    if (days < first || days > last) throw MiniSqlError(code, "DATE day number outside supported calendar range");
    // 超出范围说明数据文件里的值有问题，属于存储类错误。
    const year_month_day date{sys_days{std::chrono::days{days}}};
    // 把天数还原成日期对象。
    char text[11];
    // 十个字符加结尾零字节的缓冲。
    std::snprintf(text,sizeof(text),"%04d-%02u-%02u",static_cast<int>(date.year()),static_cast<unsigned>(date.month()),static_cast<unsigned>(date.day()));
    // 按固定宽度格式化，月份日期不足两位自动补零。
    return text;
    // 转成字符串返回。
}
inline std::optional<std::string> dateLiteralText(std::string_view raw) {
// 识别 DATE '2000-01-01' 这种字面量写法，成功时返回引号里的内容。
    if (raw.size() < 6 || (raw[0]!='D' && raw[0]!='d') || (raw[1]!='A' && raw[1]!='a') ||
        (raw[2]!='T' && raw[2]!='t') || (raw[3]!='E' && raw[3]!='e') || raw[4]!='\'' || raw.back()!='\'') return std::nullopt;
    // 前四位必须是 DATE（大小写都可），第五位是单引号，最后一位也要是单引号。
    return std::string(raw.substr(5,raw.size()-6));
    // 去掉 DATE、空格和两侧引号，返回中间日期文本。
}
} // namespace minisql
