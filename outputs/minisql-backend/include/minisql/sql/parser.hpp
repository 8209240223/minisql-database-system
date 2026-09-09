#pragma once
#include "minisql/sql/lexer.hpp"
#include <memory>
#include <variant>
#include <optional>
#include <cstdint>

namespace minisql::sql {
struct Expr { std::string kind; std::string value; std::shared_ptr<Expr> left; std::shared_ptr<Expr> right; SourceLocation location{}; std::string subquerySql{}; };
struct ColumnDef { std::string name; std::string type; bool nullable = true; std::optional<std::string> defaultValue{}; bool primaryKey = false; bool unique = false; std::optional<std::pair<std::string,std::string>> references{}; };
struct SelectItem { std::shared_ptr<Expr> expression; std::string alias; };
struct OrderItem { std::shared_ptr<Expr> expression; bool descending = false; std::optional<bool> nullsFirst{}; };
struct Assignment { std::string column; std::shared_ptr<Expr> expression; };
struct Join { std::string table; std::string alias; std::shared_ptr<Expr> on; bool left = false; bool right = false; };
struct KeyConstraint { bool primary = false; std::vector<std::string> columns{}; };
struct ForeignKey { std::vector<std::string> columns{}; std::string table{}; std::vector<std::string> referencedColumns{}; };
struct ConstraintName { std::string name; std::string kind; std::size_t index; };
struct IndexDef { std::string name; std::vector<std::string> columns; bool unique = false; };
struct Statement {
    std::string kind{};
    std::string table{};
    std::vector<ColumnDef> columns{};
    std::vector<std::string> names{};
    std::vector<std::string> values{};
    std::vector<std::string> selectList{};
    std::shared_ptr<Expr> where{};
    SourceLocation location{};
    std::vector<SelectItem> selectItems{};
    bool distinct = false;
    std::optional<std::uint64_t> limit{};
    std::uint64_t offset = 0;
    std::vector<OrderItem> orderBy{};
    std::vector<Assignment> assignments{};
    std::string tableAlias{};
    std::vector<Join> joins{};
    std::vector<std::shared_ptr<Expr>> valueExpressions{};
    bool defaultValues = false;
    std::vector<std::vector<std::shared_ptr<Expr>>> valueRows{};
    std::vector<KeyConstraint> keys{};
    std::vector<std::shared_ptr<Expr>> checks{};
    std::vector<ForeignKey> foreignKeys{};
    std::vector<ConstraintName> constraintNames{};
    std::vector<std::shared_ptr<Expr>> groupBy{};
    std::shared_ptr<Expr> having{};
    std::string indexName{};
    bool uniqueIndex = false;
    std::vector<std::string> indexColumns{};
    std::vector<IndexDef> indexes{};
    bool invalid = false;
};
inline std::string constraintSuffix(const std::vector<ConstraintName>& names, const std::string& kind, std::size_t index) {
    for (const auto& binding : names)
        if (binding.kind == kind && binding.index == index) return " [" + binding.name + "]";
    return {};
}
inline std::string foreignKeySuffix(const Statement& statement, std::size_t index) {
    if (index < statement.foreignKeys.size()) return constraintSuffix(statement.constraintNames, "foreignKey", index);
    index -= statement.foreignKeys.size();
    for (std::size_t column = 0; column < statement.columns.size(); ++column)
        if (statement.columns[column].references) {
            if (index == 0) return constraintSuffix(statement.constraintNames, "references", column);
            --index;
        }
    return {};
}
inline std::vector<ForeignKey> allForeignKeys(const Statement& statement) {
    auto result = statement.foreignKeys;
    for (const auto& column : statement.columns)
        if (column.references) result.push_back({{column.name}, column.references->first, {column.references->second}});
    return result;
}
std::vector<Statement> parse(const std::vector<Token>& tokens);
// Recovery-mode parser: instead of throwing on the first syntax error it
// collects every recoverable syntax error into `errors` (with
// endLine/endColumn spans), synchronizes to the next statement boundary, and
// marks the offending Statement invalid so callers can reject it without
// aborting the whole batch. Valid statements still come back.
std::vector<Statement> parseRecoverable(const std::vector<Token>& tokens, std::vector<MiniSqlError>& errors);
}
