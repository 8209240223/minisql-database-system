#include "minisql/security/access_catalog.hpp"
#include "minisql/common/error.hpp"
#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace minisql::security {
namespace {

constexpr std::size_t pageSize = 4096;
// 权限目录文件按固定 4096 字节一页组织，与数据页大小保持一致。
constexpr std::size_t metaHeaderBytes = 72;
// 文件头占用 72 字节：魔数、版本、页数、摘要等元信息都塞在这段里。
constexpr std::size_t dataHeaderBytes = 16;
// 每个数据页的开头 16 字节是页头（页号、长度等），正文从第 16 字节开始。
constexpr std::size_t dataCapacity = pageSize - dataHeaderBytes;
// 因此每页真正能装正文的容量是 4096 - 16 = 4080 字节。

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
// 从字节缓冲的指定偏移处按小端序读出一个 32 位整数。
    if (offset + 4 > bytes.size()) throw MiniSqlError(ErrorCode::Storage, "Access catalog header is truncated");
    // 越界说明文件被截断，按存储错误上报而不是读越界内存。
    return static_cast<std::uint32_t>(bytes[offset]) |
        // 最低字节。
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        // 次低字节左移 8 位。
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        // 次高字节左移 16 位。
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        // 最高字节左移 24 位；四段或起来就是小端序的 32 位值。
}

std::uint32_t fnv1a(const std::uint8_t* bytes, std::size_t length) {
// FNV-1a 32 位哈希，用于页级校验。
    std::uint32_t hash = 0x811c9dc5U;
    // FNV 的标准偏移基数。
    for (std::size_t index = 0; index < length; ++index) {
    // 逐字节处理。
        hash ^= bytes[index];
        // 先异或。
        hash *= 0x01000193U;
        // 再乘 32 位 FNV 素数。
    }
    return hash;
    // 返回哈希值。
}

std::uint32_t rotateRight(std::uint32_t value, unsigned amount) {
// 32 位循环右移，SHA-256 的 σ 函数要用。
    return (value >> amount) | (value << (32U - amount));
    // 右移 amount 位与左移 (32-amount) 位相或，正好把移出去的低位补到高位。
}

std::array<std::uint8_t, 32> sha256(std::string_view input) {
// 计算 SHA-256 摘要；自己实现是为了不引入第三方加密库依赖。
    constexpr std::array<std::uint32_t, 64> constants = {
    // SHA-256 规定的 64 个轮常量（前 64 个质数立方根小数部分的前 32 位）。
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        // 第 1-8 个。
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        // 第 9-16 个。
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        // 第 17-24 个。
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        // 第 25-32 个。
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        // 第 33-40 个。
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        // 第 41-48 个。
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        // 第 49-56 个。
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        // 第 57-64 个。
    };
    // 常量表结束。
    std::vector<std::uint8_t> message(input.begin(), input.end());
    // 把输入复制成可写的字节缓冲（后面要做填充，无法直接在原串上操作）。
    const auto bitLength = static_cast<std::uint64_t>(message.size()) * 8U;
    // 先记下原始消息的比特长度，填充的最后 8 字节要写它。
    message.push_back(0x80U);
    // 追加一个 0x80 字节，这是 SHA-256 填充的固定第一步。
    while (message.size() % 64U != 56U) message.push_back(0);
    // 补零直到长度对 64 取模等于 56，给最后的 8 字节长度字段留位置。
    for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<std::uint8_t>(bitLength >> shift));
    // 以大端序写入 64 位比特长度，正好 8 个字节。

    std::array<std::uint32_t, 8> state = {
    // SHA-256 的 8 个初始链接变量（前 8 个质数平方根小数部分的前 32 位）。
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        // 前四个。
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
        // 后四个。
    };
    // 状态初始化完成。
    for (std::size_t block = 0; block < message.size(); block += 64) {
    // 以 64 字节为一个分组逐块压缩。
        std::array<std::uint32_t, 64> words{};
        // 每块的 64 个 32 位工作字。
        for (std::size_t index = 0; index < 16; ++index) {
        // 前 16 个字直接由这一块的字节按大端序拼出来。
            const auto offset = block + index * 4;
            // 当前字在这一块里的字节偏移。
            words[index] = (static_cast<std::uint32_t>(message[offset]) << 24) |
                // 最高字节。
                (static_cast<std::uint32_t>(message[offset + 1]) << 16) |
                // 次高字节。
                (static_cast<std::uint32_t>(message[offset + 2]) << 8) |
                // 次低字节。
                static_cast<std::uint32_t>(message[offset + 3]);
                // 最低字节；四段拼成大端序 32 位字。
        }
        // 前 16 个字填完。
        for (std::size_t index = 16; index < words.size(); ++index) {
        // 其余 48 个字由前面的字扩展得到。
            const auto s0 = rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            // 小 σ0：对第 index-15 个字做两种循环右移与一次逻辑右移后异或。
            const auto s1 = rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            // 小 σ1：对第 index-2 个字做同样的三路异或，只是移位量不同。
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
            // 标准的消息扩展公式，加法按 32 位自然回绕。
        }
        // 扩展结束。
        auto working = state;
        // 把当前状态复制成工作变量，压缩过程中只改副本。
        for (std::size_t index = 0; index < words.size(); ++index) {
        // 64 轮压缩。
            const auto choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
            // Ch 函数：按 working[4] 的每一位，在 working[5] 与 working[6] 之间选择。
            const auto majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            // Maj 函数：三个字逐位取多数。
            const auto sum1 = rotateRight(working[4], 6) ^ rotateRight(working[4], 11) ^ rotateRight(working[4], 25);
            // 大 Σ1。
            const auto sum0 = rotateRight(working[0], 2) ^ rotateRight(working[0], 13) ^ rotateRight(working[0], 22);
            // 大 Σ0。
            const auto temporary1 = working[7] + sum1 + choice + constants[index] + words[index];
            // 第一临时值：把状态、Σ1、Ch、轮常量与当前字全部加起来。
            const auto temporary2 = sum0 + majority;
            // 第二临时值：Σ0 与 Maj 之和。
            working[7] = working[6];
            // 以下是标准的 8 个工作变量轮转：
            working[6] = working[5];
            // 逐级前移。
            working[5] = working[4];
            // 逐级前移。
            working[4] = working[3] + temporary1;
            // 第 4 个变量用旧的第 3 个加上临时值 1。
            working[3] = working[2];
            // 逐级前移。
            working[2] = working[1];
            // 逐级前移。
            working[1] = working[0];
            // 逐级前移。
            working[0] = temporary1 + temporary2;
            // 第 0 个变量是两个临时值之和。
        }
        // 本轮 64 轮结束。
        for (std::size_t index = 0; index < state.size(); ++index) state[index] += working[index];
        // 把压缩结果累加回链接变量，得到这一块的处理结果。
    }
    // 所有分组处理完毕。
    std::array<std::uint8_t, 32> result{};
    // 摘要输出缓冲。
    for (std::size_t index = 0; index < state.size(); ++index) {
    // 把 8 个 32 位状态按大端序展开成 32 字节。
        result[index * 4] = static_cast<std::uint8_t>(state[index] >> 24);
        // 每个字的最高字节。
        result[index * 4 + 1] = static_cast<std::uint8_t>(state[index] >> 16);
        // 次高字节。
        result[index * 4 + 2] = static_cast<std::uint8_t>(state[index] >> 8);
        // 次低字节。
        result[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
        // 最低字节。
    }
    // 展开结束。
    return result;
    // 返回 32 字节摘要。
}

std::string hex(const std::array<std::uint8_t, 32>& digest) {
// 把 32 字节摘要格式化成 64 个小写十六进制字符。
    constexpr char digits[] = "0123456789abcdef";
    // 十六进制字符表。
    std::string result;
    // 结果字符串。
    result.reserve(64);
    // 长度固定是 64，提前预留避免反复扩容。
    for (const auto byte : digest) {
    // 逐字节转换。
        result.push_back(digits[byte >> 4]);
        // 高 4 位对应一个字符。
        result.push_back(digits[byte & 0x0fU]);
        // 低 4 位对应一个字符。
    }
    // 转换结束。
    return result;
    // 返回十六进制文本。
}

std::string lower(std::string value) {
// 把字符串整体转成小写，用于大小写不敏感的名字比较。
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    // 逐字符转小写；入参先转无符号，避免负值传入 ctype 造成未定义行为。
    return value;
    // 返回转换后的副本。
}

std::string normalized(std::string value) {
// 先去掉首尾空白，再转小写，得到用于比较的规范形式。
    const auto first = value.find_first_not_of(" \t\r\n");
    // 找到第一个非空白字符。
    if (first == std::string::npos) return {};
    // 全是空白就返回空串。
    const auto last = value.find_last_not_of(" \t\r\n");
    // 找到最后一个非空白字符。
    return lower(value.substr(first, last - first + 1));
    // 截出有效部分并转小写。
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
// 把整个权限目录文件读成字节缓冲。
    std::ifstream stream(path, std::ios::binary);
    // 以二进制方式打开，避免换行转换破坏二进制内容。
    if (!stream) throw MiniSqlError(ErrorCode::Storage, "Cannot open access catalog: " + path.string());
    // 打不开就按存储错误上报，消息里带路径便于定位。
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    // 一次性读完全部字节。
}

std::filesystem::path accessPagesPath(const std::filesystem::path& databasePath) {
// 算出权限目录文件的路径：优先用环境变量指定，否则放在数据库同目录。
    if (const auto* configured = std::getenv("MINISQL_ACCESS_FILE"); configured && *configured) {
    // 环境变量存在且非空。
        auto path = std::filesystem::path(configured);
        // 转成路径对象。
        if (path.extension() == ".json") path.replace_extension();
        // 如果用户写的是 .json（旧格式），先把扩展名去掉。
        if (path.extension() != ".pages") path += ".pages";
        // 再补上 .pages，保证最终总是页文件格式。
        return path;
        // 返回环境变量指定的路径。
    }
    // 环境变量没设置，走默认。
    return databasePath.parent_path() / "access.catalog.pages";
    // 默认放在数据库文件所在目录下，名字固定。
}

bool constantTimeEqual(std::string_view actual, std::string_view expected) {
// 常数时间字符串比较，用于口令哈希这类不能泄漏长度/内容信息的场景。
    if (actual.size() != expected.size()) return false;
    // 长度不同直接返回；长度本身通常不是秘密，这一步是必要的短路。
    unsigned difference = 0;
    // 累积器：只要有任何一位不同，它最终就非零。
    for (std::size_t index = 0; index < actual.size(); ++index)
    // 无论是否已经发现不同，都坚持把整串比完，避免比较耗时随首个差异位置变化。
        difference |= static_cast<unsigned>(static_cast<unsigned char>(actual[index]) ^ static_cast<unsigned char>(expected[index]));
        // 逐字节异或后按位累积。
    return difference == 0;
    // 累积器仍为 0 才说明完全相等。
}

std::string firstKeyword(std::string_view source) {
// 跳过前导空白与注释，取出 SQL 的第一个关键字（小写）。
    std::size_t index = 0;
    // 扫描游标。
    for (;;) {
    // 反复跳过空白与注释，直到遇到真正的第一个词。
        while (index < source.size() && std::isspace(static_cast<unsigned char>(source[index]))) ++index;
        // 吃掉所有空白字符。
        if (index + 1 < source.size() && source[index] == '-' && source[index + 1] == '-') {
        // 遇到行注释。
            index = source.find('\n', index + 2);
            // 跳到行尾。
            if (index == std::string_view::npos) return {};
            // 没有换行说明整段都是注释，取不到关键字。
            continue;
            // 继续找下一个词。
        }
        // 行注释处理结束。
        if (index + 1 < source.size() && source[index] == '/' && source[index + 1] == '*') {
        // 遇到块注释。
            const auto end = source.find("*/", index + 2);
            // 找结束标记。
            if (end == std::string_view::npos) return {};
            // 没闭合说明注释吃到了末尾，同样取不到关键字。
            index = end + 2;
            // 跳到注释之后。
            continue;
            // 继续找下一个词。
        }
        // 块注释处理结束。
        const auto begin = index;
        // 记下词的起点。
        while (index < source.size() && std::isalpha(static_cast<unsigned char>(source[index]))) ++index;
        // 吃掉连续的字母。
        return begin == index ? std::string{} : lower(std::string(source.substr(begin, index - begin)));
        // 一个字母都没吃掉说明当前位置不是标识符，返回空串；否则返回小写关键字。
    }
    // 无限循环结束（只能从内部 return 返回）。
}

std::vector<std::string> sqlWords(std::string_view source) {
// 把 SQL 文本切成一串"词"，跳过空白与注释，字符串字面量整体丢弃。
// 用途：后续靠词序列识别 FROM/UPDATE/JOIN 后面的表名，不需要完整语法树。
    std::vector<std::string> words;
    // 结果词表。
    const auto alpha = [](char value) { return std::isalpha(static_cast<unsigned char>(value)) || value == '_'; };
    // 判断能不能作为标识符首字符。
    const auto digit = [](char value) { return std::isdigit(static_cast<unsigned char>(value)); };
    // 判断是不是数字。
    for (std::size_t index = 0; index < source.size();) {
    // 逐字符扫描。
        const char character = source[index];
        // 当前字符。
        if (std::isspace(static_cast<unsigned char>(character))) { ++index; continue; }
        // 空白直接跳过。
        if (index + 1 < source.size() && source[index] == '-' && source[index + 1] == '-') {
        // 行注释：跳到行尾。
            index += 2;
            // 跳过两个减号。
            while (index < source.size() && source[index] != '\n' && source[index] != '\r') ++index;
            // 一路吃到换行。
            continue;
            // 注释不产生词。
        }
        // 行注释分支结束。
        if (index + 1 < source.size() && source[index] == '/' && source[index + 1] == '*') {
        // 块注释：跳到结束标记之后。
            index += 2;
            // 跳过 /*。
            while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) ++index;
            // 找 */。
            index = std::min(source.size(), index + 2);
            // 跳过 */；用 min 防止越过末尾。
            continue;
            // 注释不产生词。
        }
        // 块注释分支结束。
        if (character == '\'') {
        // 字符串字面量：整个跳过，不产生词。
            ++index;
            // 跳过起始引号。
            while (index < source.size()) {
            // 直到闭合或到末尾。
                if (source[index] != '\'') { ++index; continue; }
                // 普通字符继续前进。
                if (index + 1 < source.size() && source[index + 1] == '\'') { index += 2; continue; }
                // 连续两个引号是转义，整体跳过。
                ++index;
                // 否则是结束引号。
                break;
                // 跳出。
            }
            // 字符串扫描循环结束。
            continue;
            // 字符串整体不产生词，继续扫后面。
        }
        // 字符串字面量分支结束。
        if (alpha(character)) {
        // 标识符：整段取出并转小写后收进词表。
            const auto begin = index++;
            // 记下起点并前进一格。
            while (index < source.size() && (alpha(source[index]) || digit(source[index]))) ++index;
            // 吃掉后续字母、数字、下划线。
            words.push_back(lower(std::string(source.substr(begin, index - begin))));
            // 转小写后收进词表（表名比较要大小写不敏感）。
            continue;
            // 继续扫描。
        }
        // 标识符分支结束。
        words.emplace_back(1, character);
        // 其它字符（括号、逗号、点号、运算符）各自作为单独一个"词"保留，
        // 后面识别结构时需要靠它们定位。
        ++index;
        // 前进一格。
    }
    return words;
    // 返回词表。
}

std::vector<std::string> tableReferences(std::string_view source, const std::string& keyword) {
// 词法兜底路径：源文本解析不出语法树时，靠词序列尽量把被访问的表名抽出来。
    const auto words = sqlWords(source);
    // 先把 SQL 切成词。
    std::vector<std::string> result;
    // 抽出的表名，保持出现顺序。
    std::unordered_set<std::string> ctes;
    // WITH 子句定义的 CTE 名字：它们是作用域名而不是真实对象，必须排除。
    const auto identifier = [](const std::string& value) {
    // 判断一个词像不像合法标识符。
        return !value.empty() && (std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_') &&
            // 首字符必须是字母或下划线，
            std::all_of(value.begin() + 1, value.end(), [](char character) {
            // 其余字符逐个检查。
                return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
                // 只允许字母、数字、下划线。
            });
            // 检查结束。
    };
    // identifier 定义结束。
    const auto add = [&](const std::string& value) {
    // 把候选词加进结果：必须是标识符、不是 CTE、而且还没出现过。
        if (identifier(value) && !ctes.contains(value) && std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
        // 三个条件同时满足才收，避免把关键字或重复名字当成对象。
    };
    // add 定义结束。
    const auto addAfter = [&](std::size_t index, bool allowParenthesized) {
    // 把紧跟在某个关键字后面的词当作表名收进来。
        auto cursor = index + 1;
        // 从关键字的下一个词开始看。
        if (allowParenthesized && cursor < words.size() && words[cursor] == "(") return;
        // 允许括号时：FROM (SELECT ...) 这种是派生表，本身不是对象，放弃。
        if (cursor < words.size() && words[cursor] == "lateral") ++cursor;
        // LATERAL 只是修饰词，跳过它再看真正的表名。
        if (cursor < words.size()) add(words[cursor]);
        // 把该词作为候选加进结果。
    };
    // addAfter 定义结束。
    if (!words.empty() && words.front() == "with") {
    // 语句以 WITH 开头，先把 CTE 名字收集起来。
        std::size_t cursor = words.size() > 1 && words[1] == "recursive" ? 2 : 1;
        // WITH RECURSIVE 时从第 3 个词开始，否则从第 2 个词开始。
        for (;;) {
        // 逐个处理 CTE 定义。
            if (cursor >= words.size() || !identifier(words[cursor])) break;
            // 当前词不是标识符，说明 CTE 列表已经结束。
            ctes.insert(words[cursor++]);
            // 记下这个 CTE 名字，并把游标前移。
            if (cursor + 1 >= words.size() || words[cursor] != "as" || words[cursor + 1] != "(") break;
            // CTE 定义必须是 AS ( ... )，不符合就结束扫描。
            cursor += 2;
            // 跳过 AS 与左括号。
            std::size_t depth = 1;
            // 括号深度从 1 开始。
            while (cursor < words.size() && depth) {
            // 一直找到与左括号配对的右括号。
                if (words[cursor] == "(") ++depth;
                // 遇到左括号加深。
                else if (words[cursor] == ")") --depth;
                // 遇到右括号变浅。
                ++cursor;
                // 游标前进。
            }
            // 配对结束。
            if (cursor >= words.size() || words[cursor] != ",") break;
            // 后面没有逗号说明 CTE 列表结束。
            ++cursor;
            // 跳过逗号，处理下一个 CTE。
        }
        // CTE 收集结束。
    }
    // WITH 处理结束。
    const auto effectiveKeyword = keyword == "explain"
    // 如果是 EXPLAIN，真正的语句关键字在后面，需要单独找出来。
        ? std::find_if(words.begin(), words.end(), [](const std::string& value) {
            // 找到第一个语句关键字。
            return value == "select" || value == "insert" || value == "update" || value == "delete";
            // 四种数据语句任一即命中。
        })
        // 查找结束。
        : words.end();
        // 不是 EXPLAIN 时用 end() 表示"没有额外关键字"。
    const auto command = keyword == "explain" && effectiveKeyword != words.end() ? *effectiveKeyword : keyword;
    // 得到后续规则匹配真正使用的命令词。
    for (std::size_t index = 0; index < words.size(); ++index) {
    // 从头遍历词序列找对象。
        const auto& word = words[index];
        // 当前词。
        if (word == "from" || word == "join" || word == "into" || word == "update" || word == "references") addAfter(index, true);
        // FROM/JOIN 是查询来源，INTO 是插入目标，UPDATE 是更新目标，REFERENCES 是外键父表。
        if (command == "drop" && word == "table") addAfter(index, false);
        // DROP TABLE 后面的表。
        if (command == "create" && (word == "table" || word == "on")) addAfter(index, false);
        // CREATE TABLE 与 CREATE INDEX ... ON 后面的表。
    }
    return result;
    // 返回抽出的表名。
}

bool sqlIdentifier(std::string_view value) {
// 判断一个字符串是不是合法 SQL 标识符：首字符是字母或下划线，其余是字母数字下划线。
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) return false;
    // 空串或首字符不合法直接返回假。
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
    // 检查剩余字符。
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
        // 只允许字母、数字、下划线。
    });
    // 返回检查结果。
}

void addAstObject(const std::string& value, std::vector<std::string>& result,
// 把一个候选对象名规范化后加入结果列表。
                 std::unordered_set<std::string>& seen) {
                 // seen 用于去重，保证同一个对象只出现一次。
    const auto object = normalized(value);
    // 去掉首尾空白并转小写，得到用于比较的规范名。
    if (sqlIdentifier(object) && seen.insert(object).second) result.push_back(object);
    // 名字合法且此前没见过才收进结果。
}

void collectAstStatementObjects(const sql::Statement& statement, std::vector<std::string>& result,
// 前置声明：语句级收集与表达式级收集互相递归调用。
                                std::unordered_set<std::string>& seen);
                                // 这里只是声明，定义在下面。

void collectAstExpressionObjects(const std::shared_ptr<sql::Expr>& expression,
// 遍历表达式子树，收集其中出现的数据库对象名。
                                 std::vector<std::string>& result,
                                 // 结果列表。
                                 std::unordered_set<std::string>& seen) {
                                 // 去重集合。
    if (!expression) return;
    // 空节点直接返回。
    collectAstExpressionObjects(expression->left, result, seen);
    // 先递归左子树。
    collectAstExpressionObjects(expression->right, result, seen);
    // 再递归右子树。
    if (expression->subquery) collectAstStatementObjects(*expression->subquery, result, seen);
    // 表达式挂了结构化子查询时，递归收集子查询里的对象。
    else if (!expression->subquerySql.empty()) {
    // 否则若只有子查询原文（过渡形态），改走文本解析兜底。
        try {
        // 解析可能失败，用 try 包住。
            auto nestedSql = expression->subquerySql;
            // 复制一份子查询原文。
            if (nestedSql.find_last_not_of(" \t\r\n") == std::string::npos ||
                // 全是空白，
                nestedSql[nestedSql.find_last_not_of(" \t\r\n")] != ';') nestedSql += ';';
                // 或者结尾不是分号，就补一个分号，保证能被解析成完整语句。
            for (const auto& nested : sql::parse(sql::tokenize(nestedSql)))
            // 先词法切分再语法解析。
                collectAstStatementObjects(nested, result, seen);
                // 对解析出的每条语句递归收集对象。
        } catch (const MiniSqlError&) {
        // 局部解析失败。
            // The caller will use the lexical fallback when the complete source cannot be parsed.
            // 这里故意吞掉异常：调用方会在整体解析失败时改用词法兜底路径，
            // 局部失败时"少收集一点"比直接报错更合适。
        }
    }
    // 子查询处理结束。
}

void collectAstStatementObjects(const sql::Statement& statement, std::vector<std::string>& result,
                                std::unordered_set<std::string>& seen) {
                                // 语句级收集：把一条语句引用的所有数据库对象名收进结果。
    // A derived-table alias is a scope name, not a database object. Its nested statement is collected below.
    // FROM 派生表的别名是作用域名字而不是数据库对象，所以不能当成对象；
    // 但它内部那条 SELECT 引用到的对象要在下面递归收集。
    if (!statement.table.empty() && !(statement.kind == "Select" && statement.fromSubquery))
    // 排除"SELECT 且 FROM 是派生表"的情况。
        addAstObject(statement.table, result, seen);
        // 其余情况下主表名就是真实对象。
    for (const auto& join : statement.joins) addAstObject(join.table, result, seen);
    // 连接涉及的每一张表。
    for (const auto& foreignKey : statement.foreignKeys) addAstObject(foreignKey.table, result, seen);
    // 表级外键引用的父表。
    for (const auto& column : statement.columns)
    // 遍历列定义，找列级外键。
        if (column.references) addAstObject(column.references->first, result, seen);
        // 有 REFERENCES 就收集父表名。
    if (statement.fromSubquery) collectAstStatementObjects(*statement.fromSubquery, result, seen);
    // 派生表内部语句引用的对象。
    collectAstExpressionObjects(statement.where, result, seen);
    // WHERE 里的子查询。
    collectAstExpressionObjects(statement.having, result, seen);
    // HAVING 里的子查询。
    for (const auto& item : statement.selectItems) collectAstExpressionObjects(item.expression, result, seen);
    // 投影表达式里的子查询。
    for (const auto& item : statement.orderBy) collectAstExpressionObjects(item.expression, result, seen);
    // 排序键里的子查询。
    for (const auto& item : statement.assignments) collectAstExpressionObjects(item.expression, result, seen);
    // UPDATE 赋值里的子查询。
    for (const auto& item : statement.groupBy) collectAstExpressionObjects(item, result, seen);
    // 分组键里的子查询。
    for (const auto& item : statement.checks) collectAstExpressionObjects(item, result, seen);
    // CHECK 约束里的子查询。
    for (const auto& item : statement.valueExpressions) collectAstExpressionObjects(item, result, seen);
    // INSERT 表达式值里的子查询。
    for (const auto& row : statement.valueRows)
    // INSERT 多行形式逐行处理。
        for (const auto& item : row) collectAstExpressionObjects(item, result, seen);
        // 每行的每个表达式都递归收集。
}

std::vector<std::string> astTableReferences(std::string_view source, const std::string& keyword) {
// 首选路径：先尝试把 SQL 真正解析成语法树，从树上精确收集被访问的表；
// 解析不了再退回上面的词法扫描。
    try {
    // 解析可能失败，用 try 包住。
        auto tokens = sql::tokenize(std::string(source));
        // 先做词法分析，得到 token 流。
        if (keyword == "explain" && !tokens.empty()) {
        // EXPLAIN 本身不是数据语句，需要把前缀剥掉再解析。
            const auto lowerLexeme = [](const sql::Token& token) { return normalized(token.lexeme); };
            // 局部工具：取 token 的小写文本。
            if (lowerLexeme(tokens.front()) == "explain") tokens.erase(tokens.begin());
            // 去掉开头的 EXPLAIN。
            if (!tokens.empty() && lowerLexeme(tokens.front()) == "analyze") tokens.erase(tokens.begin());
            // 再去掉可选的 ANALYZE，剩下的就是真正的语句。
        }
        // 前缀处理结束。
        std::vector<std::string> result;
        // 收集结果。
        std::unordered_set<std::string> seen;
        // 去重集合。
        for (const auto& statement : sql::parse(tokens)) collectAstStatementObjects(statement, result, seen);
        // 解析成语句列表后逐条收集对象。
        return result.empty() ? tableReferences(source, keyword) : result;
        // 解析成功但一个对象都没收集到（例如纯 SELECT 常量），仍用词法扫描补一次，
        // 保证不会因为"树上确实没有表"而漏掉本应识别的对象。
    } catch (const MiniSqlError&) {
    // 解析失败（语法不支持或语句畸形）。
        // Unsupported or malformed syntax is still checked by the conservative scanner before execution.
        // 这部分语句在执行前仍会被保守的词法扫描检查，所以这里退回词法路径而不是直接放行。
        return tableReferences(source, keyword);
        // 退回词法兜底。
    }
}

bool permissionIn(const nlohmann::json& grants, const std::string& permission, const std::string& object) {
// 判断一组授权项里是否存在"覆盖指定对象、且包含指定权限"的授权。
    if (!grants.is_array()) return false;
    // 授权列表必须是数组。
    for (const auto& grant : grants) {
    // 逐条授权检查。
        if (!grant.is_object()) continue;
        // 不是对象就跳过。
        const auto grantObject = normalized(grant.value("object", "*"));
        // 取出该授权作用的对象；缺省是 "*" 表示对所有对象生效。
        if (grantObject != "*" && grantObject != object) continue;
        // 对象不匹配就跳过（通配或精确匹配才继续）。
        const auto permissions = grant.value("permissions", nlohmann::json::array());
        // 取出该授权的权限清单。
        if (!permissions.is_array()) continue;
        // 不是数组就跳过。
        for (const auto& item : permissions) {
        // 逐个权限比较。
            if (!item.is_string()) continue;
            // 不是字符串就跳过。
            const auto value = normalized(item.get<std::string>());
            // 取规范化的权限名。
            if (value == "*" || value == permission) return true;
            // 通配权限或精确权限都算通过。
        }
        // 权限清单遍历结束。
    }
    // 授权遍历结束。
    return false;
    // 没有命中，判定无权限。
}

bool canPermission(const nlohmann::json& catalog, const std::string& user,
// 判断某个用户是否拥有指定权限：先看用户自己的授权，再看它所属角色（角色可继承）。
                   const std::string& permission, const std::string& object) {
                   // permission 是要检查的动作，object 是目标对象名。
    const auto users = catalog.value("users", nlohmann::json::object());
    // 取出 users 段。
    if (!users.is_object()) return false;
    // 不是对象说明权限目录结构不对，判定无权限。
    const auto userFound = users.find(normalized(user));
    // 按规范化的用户名查找。
    if (userFound == users.end() || !userFound->is_object()) return false;
    // 用户不存在或结构不对，判定无权限。
    if (permissionIn(userFound->value("grants", nlohmann::json::array()), permission, object)) return true;
    // 用户自身授权命中即可。
    const auto roles = catalog.value("roles", nlohmann::json::object());
    // 取出 roles 段。
    if (!roles.is_object()) return false;
    // 结构不对则无法继续判断。
    std::unordered_set<std::string> visited;
    // 记录已访问过的角色，防止继承关系成环导致无限递归。
    const std::function<bool(const std::string&)> visit = [&](const std::string& roleName) {
    // 递归访问一个角色及其父角色。
        const auto role = normalized(roleName);
        // 规范化角色名。
        if (!visited.insert(role).second) return false;
        // 已经访问过就直接返回，切断环。
        const auto found = roles.find(role);
        // 查找角色定义。
        if (found == roles.end() || !found->is_object()) return false;
        // 角色不存在或结构不对。
        if (permissionIn(found->value("grants", nlohmann::json::array()), permission, object)) return true;
        // 角色自身授权命中。
        const auto parents = found->value("inherits", nlohmann::json::array());
        // 取出它继承的父角色列表。
        if (!parents.is_array()) return false;
        // 不是数组就不继续。
        for (const auto& parent : parents) if (parent.is_string() && visit(parent.get<std::string>())) return true;
        // 递归检查每个父角色，任意一个命中即通过。
        return false;
        // 都未命中。
    };
    // visit 定义结束。
    const auto assigned = userFound->value("roles", nlohmann::json::array());
    // 取出分配给该用户的角色。
    if (!assigned.is_array()) return false;
    // 不是数组就不继续。
    for (const auto& role : assigned) if (role.is_string() && visit(role.get<std::string>())) return true;
    // 逐个角色递归判断。
    return false;
    // 用户自身与所有角色都没命中，判定无权限。
}

} // namespace

AccessCatalog AccessCatalog::load(const std::filesystem::path& databasePath) {
// 从数据库同目录的权限目录文件加载权限定义；文件不存在时保持"未启用"状态。
    AccessCatalog result;
    // 默认构造出来的对象是 enabled_ = false，即权限控制未启用。
    const auto path = accessPagesPath(databasePath);
    // 算出权限目录文件路径。
    if (!std::filesystem::exists(path)) return result;
    // 文件不存在就直接返回未启用状态，这是"老库升级"要兼容的正常情况。
    const auto bytes = readFile(path);
    // 读出全部字节。
    if (bytes.size() < pageSize * 2 || bytes.size() % pageSize != 0)
    // 至少要有"元数据页 + 一个数据页"，并且总长度必须是页大小的整数倍。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog is not a whole number of pages");
        // 不满足就按存储错误上报，说明文件被截断或不是本格式。
    if (std::string(bytes.begin(), bytes.begin() + 8) != std::string("MSQLACL\0", 8))
    // 校验文件开头的 8 字节魔数。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog magic mismatch");
        // 魔数不符说明这不是权限目录文件。
    if (read32(bytes, 8) != 1 || read32(bytes, 12) != pageSize)
    // 校验格式版本号为 1，并且页大小就是 4096。
        throw MiniSqlError(ErrorCode::Storage, "Unsupported access catalog format");
        // 任一不符都拒绝加载，避免按错误布局解读数据。
    const auto pageCount = read32(bytes, 16);
    // 读出文件里记录的总页数。
    const auto payloadBytes = read32(bytes, 20);
    // 读出有效载荷的字节数。
    result.permissionVersion_ = read32(bytes, 24);
    // 读出权限目录版本号。
    if (pageCount != bytes.size() / pageSize || pageCount < 2 || payloadBytes > (pageCount - 1) * dataCapacity)
    // 三项一致性检查：页数与实际文件长度吻合、至少两页、载荷放得下所有数据页。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog page metadata is invalid");
        // 不一致说明文件损坏或被篡改。
    std::vector<std::uint8_t> payload;
    // 待拼装的载荷。
    payload.reserve(payloadBytes);
    // 按记录的长度预留空间。
    for (std::uint32_t page = 1; page < pageCount; ++page) {
    // 从第 1 页开始逐页读取（第 0 页是元数据页）。
        const auto offset = static_cast<std::size_t>(page) * pageSize;
        // 这一页在文件中的偏移。
        if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)) != "ACLD")
        // 校验数据页魔数。
            throw MiniSqlError(ErrorCode::Storage, "Access catalog data page magic mismatch");
            // 不是 "ACLD" 说明这一页不是数据页。
        if (read32(bytes, offset + 4) != page) throw MiniSqlError(ErrorCode::Storage, "Access catalog data page id mismatch");
        // 页头里记录的页号必须和它的物理位置一致，防止页被搬运或错位。
        const auto length = read32(bytes, offset + 8);
        // 读出这一页的有效数据长度（紧跟页号之后的字段）。
        if (length > dataCapacity || read32(bytes, offset + 12) != fnv1a(bytes.data() + offset + dataHeaderBytes, length))
        // 长度不能超过单页容量，并且页内校验和必须与被校验的那段数据吻合。
            throw MiniSqlError(ErrorCode::Storage, "Access catalog data page checksum mismatch");
            // 校验失败说明数据页内容损坏。
        payload.insert(payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + dataHeaderBytes),
                       // 把这一页的正文（跳过 16 字节页头）追加到载荷末尾，
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + dataHeaderBytes + length));
                       // 到本页有效长度为止。
    }
    // 所有数据页读完。
    if (payload.size() != payloadBytes) throw MiniSqlError(ErrorCode::Storage, "Access catalog payload length mismatch");
    // 拼出来的载荷长度必须与元数据页里记录的一致，否则说明页缺失或长度字段被改过。
    const auto expected = std::string(bytes.begin() + 40, bytes.begin() + 72);
    // 元数据页偏移 40 起的 32 字节存的是载荷摘要。
    const auto actual = sha256(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    // 现场对载荷重算一份 SHA-256。
    if (!constantTimeEqual(std::string(reinterpret_cast<const char*>(actual.data()), actual.size()), expected)) {
    // 用常数时间比较，避免比较耗时泄漏摘要信息。
        // META 中保存的是原始 SHA-256 字节，而密码记录保存的是十六进制文本；这里按字节比较摘要。
        // 注意区分：这里比较的是"原始 32 字节摘要"，而用户口令记录里保存的是十六进制文本。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload digest mismatch");
        // 摘要不符说明载荷被改过。
    }
    // 完整性校验通过。
    try {
    // 解析载荷 JSON。
        result.catalog_ = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        // 从字节缓冲构造字符串再解析。
    } catch (const nlohmann::json::exception&) {
    // 解析失败。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload is not valid JSON");
        // 按存储错误上报。
    }
    // 解析结束。
    if (!result.catalog_.is_object() || !result.catalog_.contains("users") || !result.catalog_.contains("roles"))
    // 顶层结构必须是对象，并且同时包含 users 与 roles。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload is missing users or roles");
        // 缺任一段都判为无效，避免后面鉴权时拿到半截定义。
    result.enabled_ = true;
    // 全部校验通过，标记权限控制已启用。
    return result;
    // 返回加载好的权限目录。
}
// AccessCatalog::load 结束。

AccessCatalog AccessCatalog::fromDocument(nlohmann::json document, std::uint32_t permissionVersion) {
// 直接用一份已经校验过的 JSON 文档构造权限目录（数据来自系统表快照）。
    if (permissionVersion == 0 || !document.is_object() || !document.contains("users") || !document.contains("roles") ||
    // 版本号不能为 0，文档必须是对象，并且同时包含 users 与 roles 两段。
        !document.at("users").is_object() || !document.at("roles").is_object())
        // 两段还都必须是对象。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog document is invalid");
        // 任一条件不满足都判为无效文档。
    AccessCatalog result;
    // 默认构造出来是"未启用"状态。
    result.enabled_ = true;
    // 校验通过，标记为启用。
    result.permissionVersion_ = permissionVersion;
    // 记录版本号。
    result.catalog_ = std::move(document);
    // 接管文档（用 move 避免再复制一份可能很大的权限定义）。
    return result;
    // 返回构造好的权限目录。
}

bool AccessCatalog::verify(const std::string& user, const std::string& password) const {
// 认证：校验用户名与口令是否匹配。
    if (!enabled_) return true;
    // 权限控制未启用时一律放行，保持与旧行为兼容。
    const auto name = normalized(user);
    // 规范化用户名。
    const auto users = catalog_.value("users", nlohmann::json::object());
    // 取出 users 段。
    const auto found = users.is_object() ? users.find(name) : users.end();
    // 在 users 里查找该用户；结构不对时直接落到 end()。
    if (found == users.end() || !found->is_object()) return false;
    // 用户不存在或结构不对，认证失败。
    const nlohmann::json hash = found->contains("hash") ? found->at("hash") : nlohmann::json(nullptr);
    // 取出该用户的口令记录；没有 hash 字段时按 null 处理。
    if (hash.is_null()) return password.empty();
    // 记录为 null 表示该用户不设口令，此时只接受空口令。
    if (!hash.is_object() || hash.value("scheme", "") != "sha256-salted") return false;
    // 只支持 sha256-salted 一种方案；其它方案或结构不对一律拒绝，避免被误判成通过。
    const auto salt = hash.value("salt", "");
    // 取出盐值。
    const auto expected = lower(hash.value("digest", ""));
    // 取出期望摘要并转小写，便于与计算出的十六进制文本比较。
    return expected.size() == 64 && constantTimeEqual(hex(sha256(salt + ":" + password)), expected);
    // 两件事都要成立：摘要文本必须是 64 个十六进制字符；
    // 并且 sha256(盐 + ":" + 口令) 的十六进制结果要与记录一致。
    // 比较用常数时间实现，避免通过耗时反推口令内容。
}

void AccessCatalog::authorize(const std::string& user, const std::string& operation, const std::string& sql,
// 鉴权：把"操作 + SQL"映射成抽象权限，再逐个对象检查该用户是否拥有这个权限。
                              const std::string& table, const std::string& index,
                              // table / index 用于针对具体对象的操作（如索引检查）。
                              const std::vector<std::string>& resolvedObjects) const {
                              // resolvedObjects 是调用方已经解析好的真实对象列表，可能为空。
    if (!enabled_) return;
    // 权限控制未启用时直接放行。
    const auto mode = normalized(operation);
    // 规范化操作名，后面按它判断权限类别。
    const auto keyword = firstKeyword(sql);
    // 取出 SQL 的第一个关键字，用于区分 select/insert/create 等。
    std::string permission = "compile";
    // 默认权限是 compile：只编译不执行的路径（如 EXPLAIN）按它算。
    if (mode == "snapshot" || mode == "restore") permission = "checkpoint";
    // 快照与恢复等价于做检查点，需要 checkpoint 权限。
    if (mode == "catalog" || mode == "statistics" || mode == "buffer") permission = "read";
    // 目录、统计、缓冲池状态都属于只读操作。
    else if (mode == "close") permission = "connect";
    // 关闭会话需要连接权限。
    else if (mode == "indexinspect") permission = "read";
    // 索引结构检查同样只读。
    else if (keyword == "begin" || keyword == "commit" || keyword == "rollback" || keyword == "checkpoint") permission = "transaction";
    // 事务控制语句需要事务权限。
    else if (keyword == "create") permission = "create";
    // 建表建索引需要 create 权限。
    else if (keyword == "drop") permission = "drop";
    // 删对象需要 drop 权限。
    else if (keyword == "select") permission = mode == "compile" || mode == "diagnostics" ? "compile" : "select";
    // 查询语句：只是编译或诊断时按 compile 算，真正执行才需要 select 权限。
    else if (keyword == "insert" || keyword == "update" || keyword == "delete") permission = keyword;
    // 三种写语句各自对应同名权限。
    std::vector<std::string> objects;
    // 本次需要检查权限的对象列表。
    if (mode == "indexinspect" && !table.empty()) objects.push_back(normalized(table));
    // 索引检查只针对调用方给出的那一张表。
    else if (!resolvedObjects.empty()) objects = resolvedObjects;
    // 否则优先使用调用方解析好的对象列表（最准确）。
    else objects = astTableReferences(sql, keyword);
    // 都没有就现场解析 SQL 得到对象。
    if (objects.empty()) objects.push_back("*");
    // 一个对象都识别不出来时退化成检查 "*"，避免因解析失败而绕过鉴权。
    (void)index;
    // index 参数当前不参与判定，显式标记忽略以免编译告警。
    for (const auto& object : objects)
    // 逐个对象检查权限。
        if (!canPermission(catalog_, user, permission, object)) throw MiniSqlError(ErrorCode::Permission, "Permission denied");
        // 任意一个对象没权限就整体拒绝；错误信息统一为"权限不足"，不泄露具体是哪一项。
}

} // namespace minisql::security
