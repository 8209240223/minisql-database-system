#pragma once
#include <nlohmann/json.hpp>

namespace minisql {
inline nlohmann::json wireJson(nlohmann::json value) {
// 出口序列化函数：把整数按传输安全的形式整理一遍，重复调用是安全的。
    constexpr std::int64_t safe = 9007199254740991LL;
    // JavaScript 能精确表示的最大整数是 2 的 53 次方减 1，超过这个值就会丢精度。
    if (value.is_number_unsigned()) {
    // 分支一：无符号整数。
        if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(safe)) return std::to_string(value.get<std::uint64_t>());
        // 超出安全范围就转成字符串，前端收到后按字符串处理，避免精度丢失。
    } else if (value.is_number_integer()) {
    // 分支二：有符号整数。
        const auto integer = value.get<std::int64_t>();
        // 取出数值，正负都要判断。
        if (integer < -safe || integer > safe) return std::to_string(integer);
        // 绝对值超出安全范围就转成字符串。
    } else if (value.is_structured()) {
    // 分支三：数组或对象，需要递归处理内部元素。
        for (auto& child : value) child = wireJson(std::move(child));
        // 逐个元素递归调用自身，保证嵌套结构里的整数也被转换。
    }
    return value;
    // 其余类型（字符串、布尔、小数等）原样返回。
}
} // namespace minisql
