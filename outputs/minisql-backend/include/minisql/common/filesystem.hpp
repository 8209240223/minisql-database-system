#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace minisql {
inline std::filesystem::path pathFromUtf8(std::string_view text) {
// 把 UTF-8 文本转成文件系统路径；inline 保证多个编译单元包含时不冲突。
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
// 先构造 u8string 再构造 path，这样中文路径在 Windows 上不会被按本地编码解释。
}
inline std::string pathToUtf8(const std::filesystem::path& path) {
// 反向转换：把路径还原成 UTF-8 文本，用于输出到 JSON 或日志。
    const auto bytes = path.u8string();
    // 取出底层 UTF-8 字节序列。
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    // 把字节序列包装成 std::string 返回，长度用 bytes.size() 显式给出。
}
} // namespace minisql
