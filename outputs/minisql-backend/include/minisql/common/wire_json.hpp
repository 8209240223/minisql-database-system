#pragma once
#include <nlohmann/json.hpp>

namespace minisql {
// 仅在传输边界转换，内部排序、比较和去重始终使用精确整数。
inline nlohmann::json wireJson(nlohmann::json value) {
    constexpr std::int64_t safe = 9007199254740991LL;
    if (value.is_number_unsigned()) {
        if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(safe)) return std::to_string(value.get<std::uint64_t>());
    } else if (value.is_number_integer()) {
        const auto integer = value.get<std::int64_t>();
        if (integer < -safe || integer > safe) return std::to_string(integer);
    } else if (value.is_structured()) {
        for (auto& child : value) child = wireJson(std::move(child));
    }
    return value;
}
}
