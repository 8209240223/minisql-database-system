#pragma once
#include "minisql/common/error.hpp"
#include <vector>
#include <functional>
namespace minisql::sql {
struct Token { std::string type; std::string lexeme; SourceLocation location; SourceLocation endLocation{}; };
// 词法单元结构体，是词法分析器交给后续所有阶段的唯一数据载体。
// type 是种别码（KEYWORD / IDENTIFIER / INTEGER / DECIMAL / FLOAT / STRING / OPERATOR / DELIMITER / END）。
// lexeme 是词素值，即该 token 在源串里的原始文本，大小写与引号都原样保留。
// location 是起始位置，记录 token 第一个字符的行号与列号。
// endLocation 是结束位置，记录 token 之后一个字符的行列号，用于画错误区间。
std::vector<Token> tokenize(const std::string& source);
// 严格模式入口：一次性把整段 SQL 切成 token 列表，遇到第一个词法错误就抛异常。
void scanTokens(const std::string& source, const std::function<void(const Token&)>& consume);
// 流式入口：每识别出一个 token 就立刻回调一次，适合边扫描边解析，不必先把整表堆在内存里。
// 恢复模式的词法分析器：不在第一个词法错误处抛出，而是把每条词法诊断记录进 errors
// （同时带上 endLine/endColumn 构成的区间），然后在稳定位置继续扫描，让调用方
// 一次性报出整批语句里的全部词法问题；合法 token 照常返回，后面的语句仍可继续解析。
std::vector<Token> tokenizeRecoverable(const std::string& source, std::vector<MiniSqlError>& errors);
// 容错模式入口：返回结构与 tokenize 相同，区别只是错误只收集、不中断。
}
