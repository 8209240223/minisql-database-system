#include "minisql/catalog/catalog.hpp"
#include "minisql/common/error.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/decimal_type.hpp"
#include "minisql/common/decimal.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/float.hpp"
#include "minisql/common/varchar.hpp"
#include "minisql/sql/serialization.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <unordered_set>

namespace minisql::catalog {
namespace {
std::string key(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[noreturn]] void fail(const std::string& message, SourceLocation location) {
    throw MiniSqlError(ErrorCode::Semantic, message, location);
}
bool assignable(const std::string& source, const std::string& target) {
    if (stringType(source) && stringType(target)) return true;
    if (source == "null" || source == target || (target == "bigint" && source == "int")) return true;
    if (target == "float") return source == "int" || source == "bigint" || decimalType(source).has_value();
    if (source == "float") return false;
    const auto to = decimalType(target), from = decimalType(source);
    if (!to) return false;
    if (source == "int" || source == "bigint") return true;
    return from && from->scale <= to->scale && from->precision - from->scale <= to->precision - to->scale;
}

const Column& column(const Table& table, const std::string& name, SourceLocation location) {
    return table.columns[resolveColumnIndex(table, name, location)];
}

std::string literalType(const std::string& raw, SourceLocation location) {
    if (const auto date = dateLiteralText(raw)) { (void)parseIsoDate(*date,location,ErrorCode::Semantic);return "date"; }
    if (key(raw) == "null") return "null";
    if (key(raw) == "true" || key(raw) == "false") return "bool";
    if (!raw.empty() && raw.front() == '\'') return "varchar";
    if (raw.find_first_of("eE") != std::string::npos) { (void)parseFiniteFloat(raw, ErrorCode::Semantic, location);return "float"; }
    if (raw.find('.') != std::string::npos) return decimalLiteral(raw, location).type.name();
    const char* start = raw.data();
    const char* end = start + raw.size();
    if (start != end && *start == '+') ++start;
    std::int64_t value{};
    const auto result = std::from_chars(start, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        fail("Integer literal is outside INT64 range: " + raw, location);
    }
    return value < INT32_MIN || value > INT32_MAX ? "bigint" : "int";
}

std::string expressionType(const sql::Expr& expression, const Table& table,
                           SourceLocation statementLocation, std::size_t depth = 0, bool allowAggregate = false) {
    auto location = expression.location.line ? expression.location : statementLocation;
    if (depth > 256) fail("Expression depth exceeded", location);
    if (expression.kind == "Identifier") return key(column(table, expression.value, location).type);
    if (expression.kind == "Literal") return literalType(expression.value, location);
    if (expression.kind == "Exists") {
        if (expression.subquerySql.empty()) fail("EXISTS requires a subquery", location);
        return "bool";
    }
    if (expression.kind == "ScalarSubquery") {
        if (expression.subquerySql.empty()) fail("Scalar subquery requires SQL", location);
        return "null";
    }
    if (expression.kind == "InSubquery") {
        if (!expression.left || expression.subquerySql.empty()) fail("IN subquery requires a left operand and subquery", location);
        const auto operand = expressionType(*expression.left, table, statementLocation, depth + 1, false);
        if (operand != "null" && operand != "int" && operand != "bigint" && operand != "float" && !stringType(operand) && !decimalType(operand))
            fail("IN subquery operand must be a scalar value", location);
        return "bool";
    }
    if (expression.kind == "AggregateExpr") {
        if (!allowAggregate) fail("Aggregate functions are not allowed here or nested inside another aggregate", location);
        if (!expression.left || expression.right) fail("Aggregate requires one argument", location);
        const auto name = key(expression.value);
        if (name != "count" && name != "sum" && name != "avg" && name != "min" && name != "max")
            fail("Unknown aggregate function: " + expression.value, location);
        if (expression.left->kind == "Wildcard") {
            if (name != "count" || expression.left->value != "*") fail("Only COUNT accepts '*'", location);
            return "bigint";
        }
        const auto argument = expressionType(*expression.left, table, statementLocation, depth + 1, false);
        if (name == "count") return "bigint";
        if (name == "sum" || name == "avg") {
            if (const auto decimal = decimalType(argument)) return DecimalType{38, name == "sum" ? decimal->scale : std::max(6u, decimal->scale)}.name();
            if (argument == "float") return "float";
            if (argument != "int" && argument != "bigint" && argument != "null") fail("SUM/AVG require numeric arguments", location);
            return name == "sum" ? "bigint" : "decimal(38,6)";
        }
        return argument;
    }
    if (!expression.left) fail("Expression is missing its operand", location);
    const auto left = expressionType(*expression.left, table, statementLocation, depth + 1, allowAggregate);
    if (expression.kind == "Cast") {
        const auto target = key(expression.value);
        if (target == "date" || left == "date") {
            if ((target == "date" && (left == "date" || stringType(left) || left == "null")) || (left == "date" && stringType(target))) return target;
            fail("DATE CAST permits only DATE and VARCHAR", location);
        }
        if (target == "bool" || left == "bool") {
            if ((target == "bool" && (left == "bool" || stringType(left) || left == "null")) || (left == "bool" && stringType(target))) return target;
            fail("BOOL CAST permits only BOOL and VARCHAR", location);
        }
        if (target != "int" && target != "bigint" && target != "float" && !stringType(target) && !decimalType(target)) fail("Unsupported or invalid CAST target: " + target, location);
        if (left != "int" && left != "bigint" && left != "float" && !stringType(left) && left != "null" && !decimalType(left)) fail("Unsupported CAST source: " + left, location);
        return target;
    }
    if (expression.kind == "Unary" && (expression.value == "IS NULL" || expression.value == "IS NOT NULL")) return "bool";
    if (expression.kind == "Unary" && (expression.value == "+" || expression.value == "-")) {
        if (left == "float") return "float";
        if (decimalType(left)) return left;
        if (left != "int" && left != "bigint" && left != "null") fail("Unary arithmetic requires integer", location);
        return left == "bigint" ? "bigint" : "int";
    }
    if (expression.kind == "Unary" && expression.value == "NOT") {
        if (left != "bool" && left != "null") fail("NOT requires a BOOL operand", location);
        return "bool";
    }
    if (!expression.right) fail("Expression is missing its right operand", location);
    const auto right = expressionType(*expression.right, table, statementLocation, depth + 1, allowAggregate);
    const auto numeric = [](const std::string& type) { return type == "int" || type == "bigint" || type == "float" || decimalType(type).has_value(); };
    if (isArithmetic(expression.value)) {
        if ((!numeric(left) && left != "null") || (!numeric(right) && right != "null")) fail("Arithmetic requires numeric operands", location);
        if (left == "float" || right == "float") {
            if ((left != "float" && left != "null") || (right != "float" && right != "null"))
                fail("FLOAT arithmetic requires explicit CAST for mixed numeric types", location);
            return "float";
        }
        const auto a = decimalType(left), b = decimalType(right);
        if (a || b) return decimalArithmeticType(expression.value, a ? a->scale : 0, b ? b->scale : 0, location).name();
        return left == "bigint" || right == "bigint" ? "bigint" : "int";
    }
    if (expression.value == "AND" || expression.value == "OR") {
        if ((left != "bool" && left != "null") || (right != "bool" && right != "null")) fail("AND/OR require BOOL operands", location);
    } else if (left == "float" || right == "float") {
        if ((left != "float" && left != "null") || (right != "float" && right != "null"))
            fail("FLOAT comparison requires explicit CAST for mixed numeric types", location);
    } else if (left != right && left != "null" && right != "null" && !(numeric(left) && numeric(right)) && !(stringType(left) && stringType(right))) {
        fail("Comparison operands have incompatible types: " + left + " and " + right, location);
    }
    return "bool";
}
bool hasAggregate(const std::shared_ptr<sql::Expr>& expression, std::size_t depth = 0) {
    if (!expression) return false;
    if (depth > 256) fail("Expression depth exceeded", expression->location);
    return expression->kind == "AggregateExpr" || hasAggregate(expression->left, depth + 1) || hasAggregate(expression->right, depth + 1);
}
nlohmann::json groupIdentity(const sql::Expr& expression, const Table& scope, std::size_t depth = 0) {
    if (depth > 256) fail("Expression depth exceeded", expression.location);
    if (expression.kind == "Identifier")
        return {{"kind", "Identifier"}, {"columnId", resolveColumnIndex(scope, expression.value, expression.location)}};
    auto value = expression.value;
    if (expression.kind != "Literal" || value.empty() || value.front() != '\'') value = key(value);
    if (expression.kind == "Literal" && decimalType(literalType(value, expression.location))) {
        const auto decimal = decimalLiteral(value, expression.location);
        return {{"kind", "Literal"}, {"type", decimal.type.name()}, {"value", decimal.value}};
    }
    if (expression.kind == "Literal" && literalType(value, expression.location) != "varchar" && !dateLiteralText(value) &&
        value != "null" && value != "true" && value != "false") {
        const auto* begin = value.data();
        if (!value.empty() && value.front() == '+') ++begin;
        std::int64_t number{};
        const auto parsed = std::from_chars(begin, value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) fail("Invalid integer group key", expression.location);
        return {{"kind", "Literal"}, {"value", number}};
    }
    nlohmann::json result = {{"kind", expression.kind}, {"value", value}};
    if (expression.left) result["left"] = groupIdentity(*expression.left, scope, depth + 1);
    if (expression.right) result["right"] = groupIdentity(*expression.right, scope, depth + 1);
    return result;
}
void requireGrouped(const sql::Expr& expression, const Table& scope,
                    const std::unordered_set<std::string>& keys, std::size_t depth = 0) {
    if (depth > 256) fail("Expression depth exceeded", expression.location);
    if (expression.kind == "AggregateExpr" || expression.kind == "Literal") return;
    if (keys.contains(groupIdentity(expression, scope).dump())) return;
    if (expression.kind == "Identifier" || expression.kind == "Wildcard")
        fail("Column must be grouped or aggregated: " + expression.value, expression.location);
    if (expression.left) requireGrouped(*expression.left, scope, keys, depth + 1);
    if (expression.right) requireGrouped(*expression.right, scope, keys, depth + 1);
}
}

SelectAnalysis analyzeSelect(const sql::Statement& statement, const Table& scope) {
    SelectAnalysis analysis;
    analysis.aggregated = !statement.groupBy.empty() || static_cast<bool>(statement.having);
    for (const auto& item : statement.selectItems) analysis.aggregated = hasAggregate(item.expression) || analysis.aggregated;
    for (const auto& item : statement.orderBy) analysis.aggregated = hasAggregate(item.expression) || analysis.aggregated;
    std::unordered_set<std::string> keys;
    for (const auto& group : statement.groupBy) {
        if (!group) fail("Missing GROUP BY expression", statement.location);
        analysis.groupTypes.push_back(expressionType(*group, scope, statement.location));
        keys.insert(groupIdentity(*group, scope).dump());
    }
    for (const auto& item : statement.selectItems) {
        if (!item.expression) fail("Missing projection expression", statement.location);
        if (item.expression->kind == "Wildcard") {
            (void)resolveColumnName(scope, item.expression->value, item.expression->location);
            const auto dot = item.expression->value.find('.');
            for (const auto& source : scope.columns) {
                const auto qualifier = source.qualifier.empty() ? scope.name : source.qualifier;
                if (dot != std::string::npos && key(qualifier) != key(item.expression->value.substr(0, dot))) continue;
                const sql::Expr reference{"Identifier", qualifier + "." + source.name, {}, {}, item.expression->location};
                if (analysis.aggregated) requireGrouped(reference, scope, keys);
                analysis.projectionTypes.push_back(source.type);
            }
        } else {
            analysis.projectionTypes.push_back(expressionType(*item.expression, scope, statement.location, 0, true));
            if (analysis.aggregated) requireGrouped(*item.expression, scope, keys);
        }
    }
    for (const auto& item : statement.orderBy) {
        const auto expression = resolveOrder(statement, item, scope);
        (void)expressionType(*expression, scope, statement.location, 0, true);
        if (analysis.aggregated) requireGrouped(*expression, scope, keys);
    }
    if (statement.having) {
        const auto type = expressionType(*statement.having, scope, statement.location, 0, true);
        if (type != "bool" && type != "null") fail("HAVING requires a BOOL expression", statement.having->location);
        requireGrouped(*statement.having, scope, keys);
    }
    return analysis;
}

std::string resolveColumnName(const Table& table, const std::string& name, SourceLocation location) {
    const auto dot = name.find('.');
    if (dot == std::string::npos) return name;
    bool found = false;
    for (const auto& column : table.columns)
        if (key(column.qualifier.empty() ? table.name : column.qualifier) == key(name.substr(0, dot))) found = true;
    if (!found || name.find('.', dot + 1) != std::string::npos)
        fail("Unknown table qualifier: " + name.substr(0, dot), location);
    return name.substr(dot + 1);
}
std::size_t resolveColumnIndex(const Table& table, const std::string& name, SourceLocation location) {
    const auto plain = resolveColumnName(table, name, location);
    const auto dot = name.find('.');
    std::size_t match = table.columns.size();
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        const auto& column = table.columns[i];
        if (key(column.name) != key(plain)) continue;
        if (dot != std::string::npos && key(column.qualifier.empty() ? table.name : column.qualifier) != key(name.substr(0, dot))) continue;
        if (match != table.columns.size()) fail("Ambiguous column: " + name, location);
        match = i;
    }
    if (match == table.columns.size()) fail("Column does not exist: " + name, location);
    return match;
}
Table queryScope(const sql::Statement& statement, const Catalog& catalog, std::size_t joinCount) {
    const auto* first = catalog.find(statement.table);
    if (!first) fail("Table does not exist: " + statement.table, statement.location);
    Table scope{statement.tableAlias.empty() ? first->name : statement.tableAlias, first->columns};
    for (auto& column : scope.columns) column.qualifier = scope.name;
    std::unordered_set<std::string> names{key(scope.name)};
    for (std::size_t i = 0; i < std::min(joinCount, statement.joins.size()); ++i) {
        const auto& join = statement.joins[i];
        const auto* right = catalog.find(join.table);
        if (!right) fail("Table does not exist: " + join.table, statement.location);
        const auto qualifier = join.alias.empty() ? right->name : join.alias;
        if (!names.insert(key(qualifier)).second) fail("Duplicate table alias: " + qualifier, statement.location);
        if (join.right) for (auto& column : scope.columns) column.nullable = true;
        for (auto column : right->columns) { column.qualifier = qualifier;if (join.left) column.nullable = true;scope.columns.push_back(std::move(column)); }
        if (!join.on) fail("JOIN ON requires a BOOL expression", statement.location);
        const auto type = expressionType(*join.on, scope, statement.location);
        if (type != "bool" && type != "null")
            fail("JOIN ON requires a BOOL expression", statement.location);
    }
    return scope;
}
void Catalog::create(const sql::Statement& statement) {
    if (find(statement.table)) {
        throw MiniSqlError(ErrorCode::Catalog, "Table already exists: " + statement.table, statement.location);
    }
    Table table{statement.table, {}};
    std::unordered_set<std::string> names;
    bool hasPrimaryKey = false;
    for (const auto& definition : statement.columns) {
        const auto declared = key(definition.type);
        if (declared != "int" && declared != "bigint" && declared != "float" && !stringType(declared) && declared != "bool" && declared != "date" && !decimalType(declared)) fail("Unsupported or invalid column type", statement.location);
        if (definition.primaryKey) {
            if (hasPrimaryKey) fail("Multiple PRIMARY KEY declarations", statement.location);
            hasPrimaryKey = true;
            if (definition.nullable) fail("PRIMARY KEY must be NOT NULL", statement.location);
        }
        if (!names.insert(key(definition.name)).second) {
            throw MiniSqlError(ErrorCode::Catalog, "Duplicate column: " + definition.name, statement.location);
        }
        if (definition.defaultValue) {
            const auto tokens = sql::tokenize(*definition.defaultValue);
            const bool unsignedLiteral = tokens.size() == 2 && (tokens[0].type == "INTEGER" || tokens[0].type == "DECIMAL" || tokens[0].type == "FLOAT" || tokens[0].type == "STRING" ||
                key(tokens[0].lexeme) == "null" || key(tokens[0].lexeme) == "true" || key(tokens[0].lexeme) == "false");
            const bool signedLiteral = tokens.size() == 3 && (tokens[0].lexeme == "+" || tokens[0].lexeme == "-") && (tokens[1].type == "INTEGER" || tokens[1].type == "DECIMAL" || tokens[1].type == "FLOAT");
            const bool dateLiteral = tokens.size() == 3 && key(tokens[0].lexeme) == "date" && tokens[1].type == "STRING";
            if (!unsignedLiteral && !signedLiteral && !dateLiteral) fail("DEFAULT requires one literal", statement.location);
            const auto type = literalType(*definition.defaultValue, statement.location);
            if ((type == "null" && !definition.nullable) || !assignable(type, declared))
                fail("DEFAULT type mismatch for column: " + definition.name, statement.location);
            if (const auto limit=varcharLength(declared);limit && type!="null")
                validateVarchar(stringLiteralValue(*definition.defaultValue,statement.location),*limit,statement.location,ErrorCode::Semantic);
            if (const auto decimal = decimalType(declared); decimal && type != "null") {
                try { (void)ExactDecimal::parse(*definition.defaultValue, decimal->precision, decimal->scale); }
                catch (const MiniSqlError&) { fail("DEFAULT exceeds DECIMAL column precision", statement.location); }
            }
        }
        table.columns.push_back({definition.name, key(definition.type), {}, definition.nullable, definition.defaultValue, definition.primaryKey, definition.unique, definition.references});
    }
    for (const auto& constraint : statement.keys) {
        if (constraint.columns.empty()) fail("Empty key constraint", statement.location);
        if (constraint.primary && hasPrimaryKey) fail("Multiple PRIMARY KEY declarations", statement.location);
        if (constraint.primary) hasPrimaryKey = true;
        sql::KeyConstraint bound;bound.primary = constraint.primary;
        std::unordered_set<std::size_t> seenColumns;
        for (const auto& name : constraint.columns) {
            const auto index = resolveColumnIndex(table, name, statement.location);
            if (!seenColumns.insert(index).second) fail("Duplicate key column: " + name, statement.location);
            auto& column = table.columns[index];
            if (constraint.primary) {
                if (column.defaultValue && literalType(*column.defaultValue, statement.location) == "null") fail("PRIMARY KEY default cannot be NULL", statement.location);
                column.nullable = false;
            }
            bound.columns.push_back(column.name);
        }
        table.keys.push_back(std::move(bound));
    }
    const auto references = sql::allForeignKeys(statement);
    for (std::size_t r = 0; r < references.size(); ++r) {
        const auto& reference = references[r];
        if (reference.columns.empty() || reference.columns.size() != reference.referencedColumns.size())
            fail("Foreign key column counts must match and be nonempty", statement.location);
        const auto* parent = key(reference.table) == key(table.name) ? &table : find(reference.table);
        if (!parent) fail("Referenced table does not exist: " + reference.table, statement.location);
        sql::ForeignKey bound;bound.table = parent->name;
        std::unordered_set<std::size_t> childSeen, parentSeen;
        std::vector<std::size_t> parentIndices;
        for (std::size_t i = 0; i < reference.columns.size(); ++i) {
            const auto childIndex = resolveColumnIndex(table, reference.columns[i], statement.location);
            const auto parentIndex = resolveColumnIndex(*parent, reference.referencedColumns[i], statement.location);
            if (!childSeen.insert(childIndex).second || !parentSeen.insert(parentIndex).second)
                fail("Duplicate foreign key column", statement.location);
            if (key(table.columns[childIndex].type) != key(parent->columns[parentIndex].type) &&
                !(stringType(key(table.columns[childIndex].type)) && stringType(key(parent->columns[parentIndex].type))))
                fail("Foreign key types are incompatible", statement.location);
            bound.columns.push_back(table.columns[childIndex].name);
            bound.referencedColumns.push_back(parent->columns[parentIndex].name);
            parentIndices.push_back(parentIndex);
        }
        bool referencedKey = parentIndices.size() == 1 &&
            (parent->columns[parentIndices[0]].primaryKey || parent->columns[parentIndices[0]].unique);
        for (const auto& constraint : parent->keys) {
            if (constraint.columns.size() != parentIndices.size()) continue;
            bool matches = true;
            for (std::size_t i = 0; i < parentIndices.size(); ++i)
                matches = matches && key(constraint.columns[i]) == key(bound.referencedColumns[i]);
            referencedKey = referencedKey || matches;
        }
        if (!referencedKey) fail("Referenced columns must match an ordered PRIMARY KEY or UNIQUE key", statement.location);
        if (r < statement.foreignKeys.size()) table.foreignKeys.push_back(std::move(bound));
    }
    for (const auto& check : statement.checks) {
        const auto scope = Table{statement.table, table.columns, table.keys, {}};
        if (!check) fail("CHECK expression is missing", statement.location);
        const auto type = expressionType(*check, scope, statement.location);
        if (type != "bool" && type != "null") fail("CHECK must be a BOOL expression", statement.location);
        table.checks.push_back(sql::serializeExpression(check).dump());
    }
    std::unordered_set<std::string> constraintNames, namedTargets;
    for (const auto& binding : statement.constraintNames) {
        const auto tokens = sql::tokenize(binding.name);
        if (tokens.size() != 2 || tokens[0].type != "IDENTIFIER" || tokens[0].lexeme != binding.name)
            fail("Invalid constraint name", statement.location);
        if (!constraintNames.insert(key(binding.name)).second) fail("Duplicate constraint name: " + binding.name, statement.location);
        if (!namedTargets.insert(binding.kind + ":" + std::to_string(binding.index)).second)
            fail("Constraint has multiple names", statement.location);
        bool valid = (binding.kind == "key" && binding.index < table.keys.size()) ||
            (binding.kind == "check" && binding.index < table.checks.size()) ||
            (binding.kind == "foreignKey" && binding.index < table.foreignKeys.size());
        if (binding.index < table.columns.size()) {
            const auto& target = table.columns[binding.index];
            valid = valid || (binding.kind == "primaryKey" && target.primaryKey) ||
                (binding.kind == "unique" && target.unique) || (binding.kind == "references" && target.references.has_value()) ||
                (binding.kind == "notNull" && !target.nullable);
        }
        if (!valid) fail("Constraint name references an invalid target", statement.location);
        table.constraintNames.push_back(binding);
    }
    tables_.emplace(key(statement.table), std::move(table));
}

void Catalog::createIndex(const sql::Statement& statement) {
    if (statement.kind != "CreateIndex" || statement.indexName.empty() || statement.indexColumns.empty())
        fail("Invalid CREATE INDEX definition", statement.location);
    auto found = tables_.find(key(statement.table));
    if (found == tables_.end()) fail("Index table does not exist: " + statement.table, statement.location);
    auto& table = found->second;
    for (const auto& index : table.indexes)
        if (key(index.name) == key(statement.indexName)) fail("Index already exists: " + statement.indexName, statement.location);
    Index index;
    index.name = statement.indexName;
    index.unique = statement.uniqueIndex;
    for (const auto& name : statement.indexColumns) {
        const auto position = resolveColumnIndex(table, name, statement.location);
        index.columns.push_back(table.columns[position].name);
    }
    table.indexes.push_back(std::move(index));
}
void Catalog::dropIndex(const sql::Statement& statement) {
    if (statement.kind != "DropIndex" || statement.indexName.empty()) fail("Invalid DROP INDEX definition", statement.location);
    for (auto& [name, table] : tables_) {
        if (!statement.table.empty() && key(statement.table) != name) continue;
        const auto found = std::find_if(table.indexes.begin(), table.indexes.end(), [&](const Index& index) { return key(index.name) == key(statement.indexName); });
        if (found == table.indexes.end()) continue;
        table.indexes.erase(found);
        return;
    }
    fail("Index does not exist: " + statement.indexName, statement.location);
}
std::string notNullConstraintSuffix(const Table& table, std::size_t index) {
    auto suffix = sql::constraintSuffix(table.constraintNames, "notNull", index);
    if (!suffix.empty()) return suffix;
    suffix = sql::constraintSuffix(table.constraintNames, "primaryKey", index);
    if (!suffix.empty()) return suffix;
    for (std::size_t i = 0; i < table.keys.size(); ++i)
        if (table.keys[i].primary) for (const auto& name : table.keys[i].columns)
            if (key(name) == key(table.columns.at(index).name)) return sql::constraintSuffix(table.constraintNames, "key", i);
    return {};
}

const Table* Catalog::find(const std::string& name) const {
    auto it = tables_.find(key(name));
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> insertColumns(const sql::Statement& statement, const Table& table) {
    if (statement.defaultValues) {
        if (!statement.names.empty() || !statement.values.empty() || !statement.valueExpressions.empty())
            fail("DEFAULT VALUES cannot specify columns or values", statement.location);
        return {};
    }
    if (!statement.names.empty()) return statement.names;
    std::vector<std::string> names;
    for (const auto& column : table.columns) names.push_back(column.name);
    return names;
}
void validate(const std::vector<sql::Statement>& statements, Catalog& catalog) {
    for (const auto& statement : statements) {
        if (statement.kind == "Begin" || statement.kind == "Commit" || statement.kind == "Rollback" || statement.kind == "Checkpoint") continue;
        if (statement.kind == "CreateIndex") { catalog.createIndex(statement); continue; }
        if (statement.kind == "DropIndex") { catalog.dropIndex(statement); continue; }
        if (statement.kind == "Insert" && !statement.valueRows.empty()) {
            auto single = statement;
            single.valueRows.clear();
            single.values.clear();
            for (const auto& row : statement.valueRows) {
                if (row.empty()) fail("INSERT row cannot be empty", statement.location);
                single.valueExpressions = row;
                validate({single}, catalog);
            }
            continue;
        }
        if (statement.kind == "CreateTable") {
            catalog.create(statement);
            continue;
        }
        auto binding = queryScope(statement, catalog);
        const auto* table = &binding;
        if (statement.kind == "Insert") {
            const auto names = insertColumns(statement, *table);
            const auto count = statement.valueExpressions.empty() ? statement.values.size() : statement.valueExpressions.size();
            if (names.size() != count) {
                fail("INSERT column/value count mismatch", statement.location);
            }
            std::unordered_set<std::string> seen;
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto& name = names[i];
                const auto& target = column(*table, name, statement.location);
                if (!seen.insert(key(name)).second) fail("Duplicate INSERT column: " + name, statement.location);
                if (!statement.valueExpressions.empty() && !statement.valueExpressions[i]) fail("Missing INSERT expression", statement.location);
                const auto type = statement.valueExpressions.empty() ? literalType(statement.values[i], statement.location)
                    : statement.valueExpressions[i]->kind == "Default" ? literalType(target.defaultValue.value_or("NULL"), statement.location)
                    : expressionType(*statement.valueExpressions[i], Table{"", {}}, statement.location);
                if (type == "null" && !target.nullable)
                    fail("NOT NULL constraint failed" + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, name)), statement.location);
                if ((type == "null" && !target.nullable) || !assignable(type, key(target.type))) {
                    fail("INSERT value type mismatch for column: " + name, statement.location);
                }
            }
            for (const auto& target : table->columns)
                if (!seen.contains(key(target.name)) && !target.nullable && !target.defaultValue)
                    fail("Missing required INSERT column: " + target.name + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, target.name)), statement.location);
        }
        if (statement.kind == "Update") {
            std::unordered_set<std::string> seen;
            if (statement.assignments.empty()) fail("UPDATE requires assignments", statement.location);
            for (const auto& item : statement.assignments) {
                const auto& target = column(*table, item.column, statement.location);
                if (!seen.insert(key(item.column)).second) fail("Duplicate UPDATE column: " + item.column, statement.location);
                if (!item.expression) fail("Missing UPDATE expression", statement.location);
                const auto type = item.expression->kind == "Default" ? literalType(target.defaultValue.value_or("NULL"), statement.location)
                    : expressionType(*item.expression, *table, statement.location);
                if (type == "null" && !target.nullable)
                    fail("NOT NULL constraint failed" + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, item.column)), statement.location);
                if ((type == "null" && !target.nullable) || !assignable(type, key(target.type)))
                    fail("UPDATE value type mismatch for column: " + item.column, statement.location);
            }
        }
        if (statement.selectItems.empty()) for (const auto& name : statement.selectList) {
            if (resolveColumnName(*table, name, statement.location) != "*") column(*table, name, statement.location);
        }
        if (statement.kind == "Select") (void)analyzeSelect(statement, *table);
        if (statement.where) {
            const auto type = expressionType(*statement.where, *table, statement.location);
            if (type != "bool" && type != "null") fail("WHERE requires a BOOL expression", statement.location);
        }
    }
}

Catalog compileSnapshot(const std::vector<sql::Statement>& statements, const Catalog& catalog) {
    Catalog snapshot = catalog;
    validate(statements, snapshot);
    return snapshot;
}
std::shared_ptr<sql::Expr> resolveOrder(const sql::Statement& statement, const sql::OrderItem& item, const Table& table) {
    if (!item.expression) fail("Missing ORDER BY expression", statement.location);
    if (item.expression->kind != "Identifier") return item.expression;
    std::shared_ptr<sql::Expr> match;
    for (const auto& projection : statement.selectItems) {
        if (!projection.alias.empty() && key(projection.alias) == key(item.expression->value)) {
            if (match) fail("Ambiguous ORDER BY alias", item.expression->location);
            match = projection.expression;
        }
    }
    if (match) {
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
            const auto& source = table.columns[i];
            if (key(source.name) == key(item.expression->value) &&
                !(match->kind == "Identifier" && resolveColumnIndex(table, match->value, match->location) == i))
                fail("ORDER BY alias conflicts with source column", item.expression->location);
        }
        return match;
    }
    return item.expression;
}
}
