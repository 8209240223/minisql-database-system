#pragma once
#include "minisql/common/error.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace minisql {
inline std::optional<std::uint32_t> varcharLength(std::string_view type) {
    if (!type.starts_with("varchar(") || !type.ends_with(')')) return std::nullopt;
    type.remove_prefix(8);type.remove_suffix(1);
    std::uint32_t length{};
    const auto parsed=std::from_chars(type.data(),type.data()+type.size(),length);
    if (parsed.ec!=std::errc{} || parsed.ptr!=type.data()+type.size() || !length) return std::nullopt;
    return length;
}
inline bool stringType(std::string_view type) { return type=="varchar" || varcharLength(type).has_value(); }
inline std::size_t utf8Length(std::string_view value, SourceLocation location = {}, ErrorCode code = ErrorCode::Storage) {
    const auto invalid=[&](const char* message) { throw MiniSqlError(code,message,location); };
    std::size_t count=0;
    for(std::size_t i=0;i<value.size();++count) {
        auto c=static_cast<unsigned char>(value[i++]);
        if(c<0x80) continue;
        std::size_t remaining;
        std::uint32_t point,minimum;
        if(c>=0xc2 && c<=0xdf){remaining=1;point=c&0x1f;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef){remaining=2;point=c&0xf;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4){remaining=3;point=c&7;minimum=0x10000;}
        else { invalid("Invalid UTF-8 string");return 0; }
        if(remaining>value.size()-i) invalid("Truncated UTF-8 string");
        while(remaining--) {
            c=static_cast<unsigned char>(value[i++]);
            if((c&0xc0)!=0x80) invalid("Invalid UTF-8 continuation");
            point=(point<<6)|(c&0x3f);
        }
        if(point<minimum || point>0x10ffff || (point>=0xd800 && point<=0xdfff)) invalid("Invalid UTF-8 code point");
    }
    return count;
}
inline void validateVarchar(std::string_view value, std::uint32_t limit, SourceLocation location = {}, ErrorCode code = ErrorCode::Execution) {
    if (utf8Length(value,location,code)>limit) throw MiniSqlError(code,"VARCHAR value exceeds Unicode code point limit",location);
}
inline std::string stringLiteralValue(std::string_view raw, SourceLocation location = {}) {
    const auto invalid=[&]() { throw MiniSqlError(ErrorCode::Semantic,"Invalid string literal",location); };
    if(raw.size()<2 || raw.front()!='\'' || raw.back()!='\'') invalid();
    std::string result;
    for(std::size_t i=1;i+1<raw.size();++i) {
        result+=raw[i];
        if(raw[i]=='\'') {
            if(i+2>=raw.size() || raw[i+1]!='\'') invalid();
            ++i;
        }
    }
    return result;
}
}
