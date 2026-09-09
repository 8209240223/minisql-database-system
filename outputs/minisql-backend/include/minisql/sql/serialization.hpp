#pragma once
#include "minisql/sql/parser.hpp"
#include "minisql/common/decimal_type.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/varchar.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <cstdint>
#include <limits>

namespace minisql::sql {
inline nlohmann::json serializeReference(const std::optional<std::pair<std::string,std::string>>& reference) {
    return reference ? nlohmann::json{{"table", reference->first}, {"column", reference->second}} : nlohmann::json(nullptr);
}
inline nlohmann::json serializeKeys(const std::vector<KeyConstraint>& keys) {
    auto result = nlohmann::json::array();
    for (const auto& key : keys) result.push_back({{"primary", key.primary}, {"columns", key.columns}});
    return result;
}
inline nlohmann::json serializeForeignKeys(const std::vector<ForeignKey>& keys) {
    auto result = nlohmann::json::array();
    for (const auto& key : keys) result.push_back({{"columns", key.columns}, {"table", key.table}, {"referencedColumns", key.referencedColumns}});
    return result;
}
inline nlohmann::json serializeConstraintNames(const std::vector<ConstraintName>& names) {
    auto result = nlohmann::json::array();
    for (const auto& binding : names) result.push_back({{"name", binding.name}, {"kind", binding.kind}, {"index", binding.index}});
    return result;
}
inline nlohmann::json serializeExpression(const std::shared_ptr<Expr>& expression, std::size_t depth = 0) {
    if (!expression) return nullptr;
    if (depth > 256) throw MiniSqlError(ErrorCode::Syntax, "Expression depth exceeded");
    nlohmann::json node = {{"kind", expression->kind}, {"value", expression->value},
        {"line", expression->location.line}, {"column", expression->location.column}};
    if (!expression->subquerySql.empty()) node["subquerySql"] = expression->subquerySql;
    if (expression->left) node["left"] = serializeExpression(expression->left, depth + 1);
    if (expression->right) node["right"] = serializeExpression(expression->right, depth + 1);
    return node;
}
namespace detail {
inline std::shared_ptr<Expr> readCheckExpression(const nlohmann::json& node, std::size_t depth, std::size_t& remaining) {
    auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); };
    if (!node.is_object() || depth > 256 || remaining == 0) invalid();
    --remaining;
    for (const auto* field : {"kind", "value"})
        if (!node.contains(field) || !node.at(field).is_string()) invalid();
    for (const auto* field : {"line", "column"}) {
        if (!node.contains(field) || !node.at(field).is_number_integer()) invalid();
        const auto& position = node.at(field);
        if (!position.is_number_unsigned() && position.get<std::int64_t>() < 0) invalid();
        if (position.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) invalid();
    }
    auto expression = std::make_shared<Expr>();
    expression->kind = node.at("kind").get<std::string>();
    expression->value = node.at("value").get<std::string>();
    expression->location = {node.at("line").get<std::size_t>(), node.at("column").get<std::size_t>()};
    const auto& kind = expression->kind;
    const bool subqueryKind = kind == "InSubquery" || kind == "Exists" || kind == "ScalarSubquery";
    auto value = expression->value;
    if (!subqueryKind && (value.empty() || value.size() > 1048576)) invalid();
    for (char& c : value) if (c >= 'a' && c <= 'z') c -= 32;
    const bool binary = kind == "Binary";
    const bool unary = kind == "Unary" || kind == "Cast";
    if ((binary || kind == "Unary") && expression->value != value) invalid();
    if (!binary && !unary && kind != "Literal" && kind != "Identifier" && !subqueryKind) invalid();
    if (subqueryKind) {
        if (!node.contains("subquerySql") || !node.at("subquerySql").is_string()) invalid();
        expression->subquerySql = node.at("subquerySql").get<std::string>();
        if (expression->subquerySql.empty() || expression->subquerySql.size() > 1048576) invalid();
    }
    const std::size_t expectedSize = binary ? 6u : unary ? 5u : subqueryKind ? (kind == "InSubquery" ? 6u : 5u) : 4u;
    if (node.size() != expectedSize ||
        node.contains("left") != (binary || unary || kind == "InSubquery") || node.contains("right") != binary) invalid();
    if (binary && value != "AND" && value != "OR" && value != "=" && value != "!=" &&
        value != "<" && value != "<=" && value != ">" && value != ">=" &&
        value != "+" && value != "-" && value != "*" && value != "/") invalid();
    if (kind == "Unary" && value != "NOT" && value != "IS NULL" && value != "IS NOT NULL" && value != "+" && value != "-") invalid();
    if (kind == "Cast" && value != "INT" && value != "BIGINT" && value != "FLOAT" && value != "VARCHAR" && value != "BOOL" && value != "DATE") {
        auto type = value;
        for (char& c : type) if (c >= 'A' && c <= 'Z') c += 32;
        if (!minisql::decimalType(type) && !minisql::varcharLength(type)) invalid();
    }
    if (!binary && !unary && !subqueryKind) {
        const auto tokens = tokenize(expression->value);
        std::string joined;
        for (const auto& token : tokens) joined += token.lexeme;
        if (joined != expression->value) invalid();
        if (kind == "Identifier") {
            if (!((tokens.size() == 2 && tokens[0].type == "IDENTIFIER") ||
                (tokens.size() == 4 && tokens[0].type == "IDENTIFIER" && tokens[1].lexeme == "." && tokens[2].type == "IDENTIFIER"))) invalid();
        } else {
            const bool single = tokens.size() == 2 && (tokens[0].type == "INTEGER" || tokens[0].type == "DECIMAL" || tokens[0].type == "FLOAT" || tokens[0].type == "STRING" || value == "NULL" || value == "TRUE" || value == "FALSE");
            const bool signedInteger = tokens.size() == 3 && (tokens[0].lexeme == "+" || tokens[0].lexeme == "-") && (tokens[1].type == "INTEGER" || tokens[1].type == "DECIMAL" || tokens[1].type == "FLOAT");
            const bool dateLiteral = tokens.size() == 3 && value.starts_with("DATE'") && tokens[1].type == "STRING";
            if (dateLiteral) { const auto date = dateLiteralText(value);if (!date) invalid();(void)parseIsoDate(*date); }
            if (!single && !signedInteger && !dateLiteral) invalid();
        }
    }
    if (binary || unary || kind == "InSubquery") expression->left = readCheckExpression(node.at("left"), depth + 1, remaining);
    if (binary) expression->right = readCheckExpression(node.at("right"), depth + 1, remaining);
    return expression;
}
}
inline std::shared_ptr<Expr> deserializeExpression(const nlohmann::json& node, std::size_t depth = 0) {
    std::size_t remaining = 65536;
    try { return detail::readCheckExpression(node, depth, remaining); }
    catch (const MiniSqlError&) { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); }
    catch (const nlohmann::json::exception&) { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); }
}
inline nlohmann::json serializeChecks(const std::vector<std::shared_ptr<Expr>>& checks) {
    auto result = nlohmann::json::array();
    for (const auto& check : checks) result.push_back(serializeExpression(check));
    return result;
}
inline nlohmann::json serializeStatement(const Statement& statement) {
    using json = nlohmann::json;
    json node = {{"kind", statement.kind}, {"table", statement.table}, {"tableAlias", statement.tableAlias}, {"indexName", statement.indexName}, {"uniqueIndex", statement.uniqueIndex}, {"indexColumns", statement.indexColumns}, {"selectList", statement.selectList},
        {"names", statement.names}, {"values", statement.values}, {"where", serializeExpression(statement.where)},
        {"line", statement.location.line}, {"column", statement.location.column},
        {"columns", json::array()}, {"selectItems", json::array()}, {"orderBy", json::array()},
        {"distinct", statement.distinct}, {"limit", statement.limit ? json(std::to_string(*statement.limit)) : json(nullptr)},
        {"offset", std::to_string(statement.offset)}};
    for (const auto& column : statement.columns)
        node["columns"].push_back({{"name", column.name}, {"type", column.type}, {"nullable", column.nullable},
            {"defaultValue", column.defaultValue ? json(*column.defaultValue) : json(nullptr)}, {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", serializeReference(column.references)}});
    for (const auto& item : statement.selectItems)
        node["selectItems"].push_back({{"expression", serializeExpression(item.expression)}, {"alias", item.alias}});
    for (const auto& item : statement.orderBy)
        node["orderBy"].push_back({{"expression", serializeExpression(item.expression)}, {"descending", item.descending}, {"nullsFirst", item.nullsFirst ? json(*item.nullsFirst) : json(nullptr)}});
    node["assignments"] = json::array();
    node["groupBy"] = serializeChecks(statement.groupBy);
    node["having"] = serializeExpression(statement.having);
    node["valueExpressions"] = json::array();
    node["defaultValues"] = statement.defaultValues;
    node["valueRows"] = json::array();
    for (const auto& row : statement.valueRows) {
        auto values = json::array();
        for (const auto& expression : row) values.push_back(serializeExpression(expression));
        node["valueRows"].push_back(std::move(values));
    }
    node["keys"] = serializeKeys(statement.keys);
    node["foreignKeys"] = serializeForeignKeys(statement.foreignKeys);
    node["constraintNames"] = serializeConstraintNames(statement.constraintNames);
    node["checks"] = serializeChecks(statement.checks);
    for (const auto& expression : statement.valueExpressions)
        node["valueExpressions"].push_back(serializeExpression(expression));
    for (const auto& item : statement.assignments)
        node["assignments"].push_back({{"column", item.column}, {"expression", serializeExpression(item.expression)}});
    node["joins"] = json::array();
    for (const auto& join : statement.joins)
        node["joins"].push_back({{"kind", join.left && join.right ? "FullJoin" : join.left ? "LeftJoin" : join.right ? "RightJoin" : "InnerJoin"}, {"table", join.table}, {"alias", join.alias}, {"on", serializeExpression(join.on)}});
    return node;
}
namespace detail {
inline Statement readStatement(const nlohmann::json& node, std::size_t depth = 0) {
    auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized AST statement"); };
    if (!node.is_object() || depth > 64) invalid();
    const auto stringField = [&](const char* name) -> std::string {
        if (!node.contains(name) || !node.at(name).is_string()) invalid();
        return node.at(name).get<std::string>();
    };
    const auto strings = [&](const char* name) -> std::vector<std::string> {
        if (!node.contains(name) || !node.at(name).is_array()) invalid();
        std::vector<std::string> result;
        for (const auto& value : node.at(name)) { if (!value.is_string()) invalid(); result.push_back(value.get<std::string>()); }
        return result;
    };
    const auto expression = [&](const char* name) -> std::shared_ptr<Expr> {
        if (!node.contains(name) || node.at(name).is_null()) return {};
        return deserializeExpression(node.at(name), depth + 1);
    };
    Statement statement;
    statement.kind = stringField("kind");
    statement.table = stringField("table");
    statement.tableAlias = node.value("tableAlias", std::string{});
    statement.indexName = node.value("indexName", std::string{});
    statement.uniqueIndex = node.value("uniqueIndex", false);
    statement.indexColumns = node.value("indexColumns", std::vector<std::string>{});
    statement.selectList = strings("selectList");
    statement.names = strings("names");
    statement.values = strings("values");
    statement.where = expression("where");
    statement.location = {node.value("line", std::size_t{0}), node.value("column", std::size_t{0})};
    statement.distinct = node.value("distinct", false);
    if (node.contains("limit") && !node.at("limit").is_null()) {
        if (!node.at("limit").is_string()) invalid();
        std::uint64_t value{};
        const auto text = node.at("limit").get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
        statement.limit = value;
    }
    if (node.contains("offset")) {
        if (!node.at("offset").is_string()) invalid();
        const auto text = node.at("offset").get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), statement.offset);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
    }
    if (node.contains("columns")) for (const auto& item : node.at("columns")) {
        if (!item.is_object() || !item.contains("name") || !item.contains("type")) invalid();
        ColumnDef column{item.at("name").get<std::string>(), item.at("type").get<std::string>()};
        column.nullable = item.value("nullable", true);
        column.primaryKey = item.value("primaryKey", false);
        column.unique = item.value("unique", false);
        if (item.contains("defaultValue") && !item.at("defaultValue").is_null()) column.defaultValue = item.at("defaultValue").get<std::string>();
        if (item.contains("references") && !item.at("references").is_null()) {
            if (!item.at("references").is_object()) invalid();
            column.references = std::make_pair(item.at("references").at("table").get<std::string>(), item.at("references").at("column").get<std::string>());
        }
        statement.columns.push_back(std::move(column));
    }
    if (node.contains("selectItems")) for (const auto& item : node.at("selectItems")) {
        if (!item.is_object()) invalid();
        statement.selectItems.push_back({item.contains("expression") ? deserializeExpression(item.at("expression"), depth + 1) : std::shared_ptr<Expr>{}, item.value("alias", std::string{})});
    }
    if (node.contains("orderBy")) for (const auto& item : node.at("orderBy")) {
        if (!item.is_object() || !item.contains("expression")) invalid();
        OrderItem order{deserializeExpression(item.at("expression"), depth + 1), item.value("descending", false)};
        if (item.contains("nullsFirst") && !item.at("nullsFirst").is_null()) order.nullsFirst = item.at("nullsFirst").get<bool>();
        statement.orderBy.push_back(std::move(order));
    }
    if (node.contains("assignments")) for (const auto& item : node.at("assignments")) {
        if (!item.is_object() || !item.contains("column") || !item.contains("expression")) invalid();
        statement.assignments.push_back({item.at("column").get<std::string>(), deserializeExpression(item.at("expression"), depth + 1)});
    }
    if (node.contains("joins")) for (const auto& item : node.at("joins")) {
        if (!item.is_object() || !item.contains("table") || !item.contains("on")) invalid();
        Join join;
        join.table = item.at("table").get<std::string>();
        join.alias = item.value("alias", std::string{});
        const auto kind = item.value("kind", "InnerJoin");
        join.left = kind == "LeftJoin" || kind == "FullJoin";
        join.right = kind == "RightJoin" || kind == "FullJoin";
        join.on = deserializeExpression(item.at("on"), depth + 1);
        statement.joins.push_back(std::move(join));
    }
    if (node.contains("valueExpressions")) for (const auto& item : node.at("valueExpressions")) statement.valueExpressions.push_back(deserializeExpression(item, depth + 1));
    statement.defaultValues = node.value("defaultValues", false);
    if (node.contains("valueRows")) for (const auto& row : node.at("valueRows")) {
        if (!row.is_array()) invalid();
        std::vector<std::shared_ptr<Expr>> values;
        for (const auto& item : row) values.push_back(deserializeExpression(item, depth + 1));
        statement.valueRows.push_back(std::move(values));
    }
    if (node.contains("keys")) for (const auto& item : node.at("keys")) {
        if (!item.is_object()) invalid();
        statement.keys.push_back({item.value("primary", false), item.at("columns").get<std::vector<std::string>>()});
    }
    if (node.contains("checks")) for (const auto& item : node.at("checks")) statement.checks.push_back(deserializeExpression(item, depth + 1));
    if (node.contains("foreignKeys")) for (const auto& item : node.at("foreignKeys")) {
        if (!item.is_object()) invalid();
        statement.foreignKeys.push_back({item.at("columns").get<std::vector<std::string>>(), item.at("table").get<std::string>(), item.at("referencedColumns").get<std::vector<std::string>>()});
    }
    if (node.contains("constraintNames")) for (const auto& item : node.at("constraintNames")) {
        if (!item.is_object() || !item.at("index").is_number_unsigned()) invalid();
        statement.constraintNames.push_back({item.at("name").get<std::string>(), item.at("kind").get<std::string>(), item.at("index").get<std::size_t>()});
    }
    if (node.contains("groupBy")) for (const auto& item : node.at("groupBy")) statement.groupBy.push_back(deserializeExpression(item, depth + 1));
    statement.having = expression("having");
    return statement;
}
}
inline std::vector<Statement> deserializeAst(const nlohmann::json& document) {
    nlohmann::json nodes = document;
    if (document.is_object() && document.contains("schemaVersion")) {
        if (document.at("schemaVersion") != 1 || !document.contains("statements") || !document.at("statements").is_array())
            throw MiniSqlError(ErrorCode::Storage, "Unsupported AST schema version");
        nodes = document.at("statements");
    }
    if (nodes.is_object()) nodes = nlohmann::json::array({nodes});
    if (!nodes.is_array() || nodes.size() > 1024) throw MiniSqlError(ErrorCode::Storage, "Invalid serialized AST document");
    std::vector<Statement> statements;
    for (const auto& node : nodes) statements.push_back(detail::readStatement(node));
    return statements;
}
inline nlohmann::json serializeAst(const std::vector<Statement>& statements) {
    auto nodes = nlohmann::json::array();
    for (const auto& statement : statements) nodes.push_back(serializeStatement(statement));
    return nodes.size() == 1 ? nodes[0] : nodes;
}
inline nlohmann::json serializeTokens(const std::vector<Token>& tokens) {
    auto nodes = nlohmann::json::array();
    for (const auto& token : tokens) nodes.push_back({{"type", token.type}, {"text", token.lexeme},
        {"line", token.location.line}, {"column", token.location.column}});
    return nodes;
}
}
