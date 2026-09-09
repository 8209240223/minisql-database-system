#pragma once
#include "minisql/common/error.hpp"
#include <vector>
#include <functional>
namespace minisql::sql {
struct Token { std::string type; std::string lexeme; SourceLocation location; };
std::vector<Token> tokenize(const std::string& source);
void scanTokens(const std::string& source, const std::function<void(const Token&)>& consume);
}
