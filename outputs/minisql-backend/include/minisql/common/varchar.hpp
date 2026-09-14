#pragma once
#include "minisql/common/error.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace minisql {
inline std::optional<std::uint32_t> varcharLength(std::string_view type) {
// 从类型名里取出长度上限：varchar(40) 得到 40，其他形式返回空。
    if (!type.starts_with("varchar(") || !type.ends_with(')')) return std::nullopt;
    // 必须形如 varchar(...)，否则不是带长度的类型。
    type.remove_prefix(8);type.remove_suffix(1);
    // 去掉前缀 varchar( 共八个字符，再去掉结尾的右括号。
    std::uint32_t length{};
    // 存放解析出的长度。
    const auto parsed=std::from_chars(type.data(),type.data()+type.size(),length);
    // 把括号里的内容解析成整数。
    if (parsed.ec!=std::errc{} || parsed.ptr!=type.data()+type.size() || !length) return std::nullopt;
    // 解析失败、有剩余字符、或者长度为零，都判为非法类型。
    return length;
    // 返回合法的长度上限。
}
inline bool stringType(std::string_view type) { return type=="varchar" || varcharLength(type).has_value(); }
// 判断是不是字符串类型：不带长度的 varchar 与带长度的 varchar(n) 都算。
inline std::size_t utf8Length(std::string_view value, SourceLocation location = {}, ErrorCode code = ErrorCode::Storage) {
// 手写 UTF-8 解码，统计字符数而不是字节数；varchar(n) 里的 n 指的是字符数。
    const auto invalid=[&](const char* message) { throw MiniSqlError(code,message,location); };
    // 统一的失败出口，越界、截断、非法码点都走它。
    std::size_t count=0;
    // 已统计到的字符个数。
    for(std::size_t i=0;i<value.size();++count) {
    // 每成功解析出一个码点就加一。
        auto c=static_cast<unsigned char>(value[i++]);
        // 取首字节并推进下标；用无符号类型避免高位被当负数。
        if(c<0x80) continue;
        // 首字节小于 0x80 说明是 ASCII，一个字节即一个字符。
        std::size_t remaining;
        // 该字符还剩几个后续字节。
        std::uint32_t point,minimum;
        // 累积出的码点，以及该长度下允许的最小码点。
        if(c>=0xc2 && c<=0xdf){remaining=1;point=c&0x1f;minimum=0x80;}
        // 两字节序列：首字节范围与掩码。
        else if(c>=0xe0 && c<=0xef){remaining=2;point=c&0xf;minimum=0x800;}
        // 三字节序列。
        else if(c>=0xf0 && c<=0xf4){remaining=3;point=c&7;minimum=0x10000;}
        // 四字节序列。
        else { invalid("Invalid UTF-8 string");return 0; }
        // 其他首字节都是非法 UTF-8。
        if(remaining>value.size()-i) invalid("Truncated UTF-8 string");
        // 剩余字节不够，说明字符串被截断。
        while(remaining--) {
        // 逐个读取后续字节。
            c=static_cast<unsigned char>(value[i++]);
            // 取下一个字节。
            if((c&0xc0)!=0x80) invalid("Invalid UTF-8 continuation");
            // 后续字节必须形如 10xxxxxx。
            point=(point<<6)|(c&0x3f);
            // 把六个有效位并入码点。
        }
        if(point<minimum || point>0x10ffff || (point>=0xd800 && point<=0xdfff)) invalid("Invalid UTF-8 code point");
        // 拒绝过长编码、超出 Unicode 上限，以及代理区码点。
    }
    return count;
    // 返回字符总数。
}
inline void validateVarchar(std::string_view value, std::uint32_t limit, SourceLocation location = {}, ErrorCode code = ErrorCode::Execution) {
// 校验字符串的字符数是否超过声明的上限。
    if (utf8Length(value,location,code)>limit) throw MiniSqlError(code,"VARCHAR value exceeds Unicode code point limit",location);
    // 超限就报错；注意比较的是字符数而不是字节数。
}
inline std::string stringLiteralValue(std::string_view raw, SourceLocation location = {}) {
// 处理字符串字面量：去掉两侧单引号，并把连续两个单引号还原成一个。
    const auto invalid=[&]() { throw MiniSqlError(ErrorCode::Semantic,"Invalid string literal",location); };
    // 字面量格式不合法属于语义错误。
    if(raw.size()<2 || raw.front()!='\'' || raw.back()!='\'') invalid();
    // 必须首尾都是单引号，且至少两个字符。
    std::string result;
    // 解码后的结果。
    for(std::size_t i=1;i+1<raw.size();++i) {
    // 从第二个字符扫到倒数第二个字符。
        result+=raw[i];
        // 先原样收下当前字符。
        if(raw[i]=='\'') {
        // 遇到单引号要判断是不是转义写法。
            if(i+2>=raw.size() || raw[i+1]!='\'') invalid();
            // 后面必须紧跟第二个单引号，否则就是非法的孤立引号。
            ++i;
            // 跳过第二个单引号，把它算作一个引号字符。
        }
    }
    return result;
    // 返回解码后的字符串内容。
}
} // namespace minisql
