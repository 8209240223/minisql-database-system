#pragma once
#include "minisql/common/error.hpp"
#include <vector>
#include <functional>
namespace minisql::sql {
struct Token { std::string type; std::string lexeme; SourceLocation location; SourceLocation endLocation{}; };
std::vector<Token> tokenize(const std::string& source);
void scanTokens(const std::string& source, const std::function<void(const Token&)>& consume);
// Recovery-mode tokenizer: instead of throwing on the first lexical error it
// records every lexical diagnostic into `errors` (with `endLine/endColumn`
// spans) and resumes scanning at a stable token so the caller can report them
// all at once. Valid tokens still come back so later statements parse.
std::vector<Token> tokenizeRecoverable(const std::string& source, std::vector<MiniSqlError>& errors);
}
