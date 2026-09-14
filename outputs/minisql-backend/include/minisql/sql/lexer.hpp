#pragma once
#include "minisql/common/error.hpp"
#include <vector>
#include <functional>
namespace minisql::sql {
struct Token {
    std::string type;
    std::string lexeme;
    SourceLocation location;
    SourceLocation endLocation{};
    std::size_t byteStart = 0;
    std::size_t byteEnd = 0;
};
std::vector<Token> tokenize(const std::string& source);
// 严格模式入口：一次性把整段 SQL 切成 token 列表，遇到第一个词法错误就抛异常。
void scanTokens(const std::string& source, const std::function<void(const Token&)>& consume);
// 流式入口：每识别出一个 token 就立刻回调一次，适合边扫描边解析，不必先把整表堆在内存里。
// 恢复模式的词法分析器：不在第一个词法错误处抛出，而是把每条词法诊断记录进 errors
// （同时带上 endLine/endColumn 构成的区间），然后在稳定位置继续扫描，让调用方
// 一次性报出整批语句里的全部词法问题；合法 token 照常返回，后面的语句仍可继续解析。
// Recovery-mode tokenizer: instead of throwing on the first lexical error it
// records every lexical diagnostic into `errors` (with `endLine/endColumn`
// spans) and resumes scanning at a stable token so the caller can report them
// all at once. Valid tokens still come back so later statements parse.
std::vector<Token> tokenizeRecoverable(const std::string& source, std::vector<MiniSqlError>& errors);
// 容错模式入口：返回结构与 tokenize 相同，区别只是错误只收集、不中断。
}
