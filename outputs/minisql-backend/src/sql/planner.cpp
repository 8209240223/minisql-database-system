#include "minisql/sql/planner.hpp"
#include "minisql/sql/serialization.hpp"
#include "minisql/sql/parser.hpp"
#include "minisql/sql/lexer.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/decimal_type.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/float.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace minisql::sql {
namespace {
std::string canonical(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// X09 4.x: 相关子查询去相关 —— 编译右子计划时，未被自身表作用域解析的“限定名
// （外层别名.列）”落到关联作用域，生成 Parameter 节点（compiled-once 参数化访问）。
// 以 thread_local 传参避免侵入所有 bindExpression/... 签名；RAII 守卫保证每次
// compile 结束后复位，不跨语句泄漏。
using CorrelatedScope = std::unordered_map<std::string, std::pair<std::size_t, std::string>>;
thread_local const CorrelatedScope* activeCorrelated = nullptr;
struct CorrelatedScopeGuard {
    const CorrelatedScope* previous;
    explicit CorrelatedScopeGuard(const CorrelatedScope* scope) : previous(activeCorrelated) { activeCorrelated = scope; }
    ~CorrelatedScopeGuard() { activeCorrelated = previous; }
};

[[noreturn]] void invalid(const std::string& message) {
    throw MiniSqlError(ErrorCode::Internal, "Plan invariant: " + message);
}

std::size_t columnIndex(const catalog::Table& table, const std::string& name) {
    return catalog::resolveColumnIndex(table, name);
}

nlohmann::json literalValue(const std::string& raw) {
    if (const auto date = dateLiteralText(raw)) return formatIsoDate(parseIsoDate(*date));
    if (canonical(raw) == "null") return nullptr;
    if (canonical(raw) == "true" || canonical(raw) == "false") return canonical(raw) == "true";
    if (!raw.empty() && raw.front() == '\'') {
        return stringLiteralValue(raw);
    }
    if (raw.find_first_of("eE") != std::string::npos) return parseFiniteFloat(raw, ErrorCode::Internal);
    if (raw.find('.') != std::string::npos) return decimalLiteral(raw).value;
    const auto* start = raw.data();
    const auto* end = start + raw.size();
    if (start != end && *start == '+') ++start;
    std::int64_t value{};
    const auto result = std::from_chars(start, end, value);
    if (result.ec != std::errc{} || result.ptr != end) invalid("unchecked integer literal");
    return value;
}

nlohmann::json correlatedScope(const catalog::Table& table) {
    nlohmann::json scope = nlohmann::json::object();
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        const auto& column = table.columns[index];
        if (column.qualifier.empty()) continue;
        scope[canonical(column.qualifier + "." + column.name)] = {{"columnId", index}, {"type", column.type}};
    }
    return scope;
}
nlohmann::json bindExpression(const Expr& expression, const catalog::Table& table, std::size_t depth = 0) {
    if (depth > 256) invalid("expression depth exceeded");
    nlohmann::json result = {{"kind", expression.kind}, {"line", expression.location.line},
                             {"column", expression.location.column}};
    if (expression.kind == "Identifier") {
        // X09 4.x: 编译相关子查询的右子计划时，外层的“限定名.列”未在当前表作用域内，
        // 落入活动关联作用域则生成 Parameter；否则照常按表作用域解析。
        if (activeCorrelated) {
            const auto outer = activeCorrelated->find(canonical(expression.value));
            if (outer != activeCorrelated->end()) {
                result["kind"] = "Parameter";
                result["paramId"] = outer->second.first;
                result["type"] = outer->second.second;
                result["nullable"] = true;
                return result;
            }
        }
        const auto index = columnIndex(table, expression.value);
        result["columnId"] = index;
        result["name"] = table.columns[index].name;
        result["type"] = table.columns[index].type;
        result["nullable"] = table.columns[index].nullable;
    } else if (expression.kind == "Literal") {
        result["value"] = literalValue(expression.value);
        result["type"] = result["value"].is_null() ? "null" : result["value"].is_boolean() ? "bool" : result["value"].is_string() ? "varchar" : result["value"].is_number_float() ? "float" : "int";
        if (result["value"].is_number_integer() && (result["value"].get<std::int64_t>() < INT32_MIN || result["value"].get<std::int64_t>() > INT32_MAX)) result["type"] = "bigint";
        result["nullable"] = result["value"].is_null();
        // A DATE literal (`DATE '2000-01-01'`) carries an ISO string value, so the
        // numeric heuristics below must not run on it. The 'E' in the DATE keyword
        // would otherwise classify it as a FLOAT literal whose value stays a
        // string, and comparing it later fails with an internal type error.
        const bool isDateLiteral = dateLiteralText(expression.value).has_value();
        if (isDateLiteral) result["type"] = "date";
        if (!isDateLiteral && expression.value.find_first_of("eE") != std::string::npos && expression.value.front() != '\'') result["type"] = "float";
        if (!isDateLiteral && expression.value.find_first_of("eE") == std::string::npos && expression.value.find('.') != std::string::npos && expression.value.front() != '\'') result["type"] = decimalLiteral(expression.value, expression.location).type.name();
    } else if (expression.kind == "Exists") {
        if (expression.subquerySql.empty()) invalid("missing EXISTS subquery");
        result["subquerySql"] = expression.subquerySql;
        result["outerColumns"] = correlatedScope(table);
        result["type"] = "bool";
        result["nullable"] = false;
    } else if (expression.kind == "ScalarSubquery") {
        if (expression.subquerySql.empty()) invalid("missing scalar subquery");
        result["subquerySql"] = expression.subquerySql;
        result["outerColumns"] = correlatedScope(table);
        result["type"] = "null";
        result["nullable"] = true;
    } else if (expression.kind == "InSubquery") {
        if (!expression.left || expression.subquerySql.empty()) invalid("missing IN subquery operand");
        result["left"] = bindExpression(*expression.left, table, depth + 1);
        result["subquerySql"] = expression.subquerySql;
        result["outerColumns"] = correlatedScope(table);
        result["type"] = "bool";
        result["nullable"] = true;
    } else if (expression.kind == "AggregateExpr") {
        if (!expression.left) invalid("missing aggregate argument");
        result["function"] = expression.value;
        result["left"] = expression.left->kind == "Wildcard" ? nlohmann::json(nullptr) : bindExpression(*expression.left, table, depth + 1);
        result["type"] = expression.value == "COUNT" || expression.value == "SUM" ? "bigint" :
            expression.value == "AVG" ? "decimal(38,6)" : result["left"].at("type").get<std::string>();
        result["nullable"] = expression.value != "COUNT";
        if (expression.value == "SUM" || expression.value == "AVG") {
            if (result["left"].at("type") == "float") result["type"] = "float";
            else if (const auto decimal = decimalType(result["left"].at("type").get<std::string>()))
                result["type"] = DecimalType{38, expression.value == "SUM" ? decimal->scale : std::max(6u, decimal->scale)}.name();
        }
    } else if (expression.kind == "Cast") {
        if (!expression.left) invalid("missing CAST operand");
        result["type"] = canonical(expression.value);
        result["left"] = bindExpression(*expression.left, table, depth + 1);
        result["nullable"] = result["left"].value("nullable", true);
    } else if (expression.kind == "Unary" || expression.kind == "Binary") {
        result["operator"] = expression.value;
        result["type"] = isArithmetic(expression.value) ? "int" : "bool";
        if (!expression.left) invalid("missing left operand");
        result["left"] = bindExpression(*expression.left, table, depth + 1);
        if (expression.kind == "Binary") {
            if (!expression.right) invalid("missing right operand");
            result["right"] = bindExpression(*expression.right, table, depth + 1);
        }
        result["nullable"] = expression.value != "IS NULL" && expression.value != "IS NOT NULL" &&
            (result["left"].value("nullable", true) || (expression.kind == "Binary" && result["right"].value("nullable", true)));
        if (isArithmetic(expression.value) && (result["left"].at("type") == "bigint" || (expression.kind == "Binary" && result["right"].at("type") == "bigint"))) result["type"] = "bigint";
        if (isArithmetic(expression.value)) {
            if (result["left"].at("type") == "float" || (expression.kind == "Binary" && result["right"].at("type") == "float")) {
                result["type"] = "float";
            } else {
            const auto a = decimalType(result["left"].at("type").get<std::string>());
            const auto b = expression.kind == "Binary" ? decimalType(result["right"].at("type").get<std::string>()) : std::nullopt;
            if (expression.kind == "Unary" && a) result["type"] = a->name();
            else if (a || b) result["type"] = decimalArithmeticType(expression.value, a ? a->scale : 0, b ? b->scale : 0, expression.location).name();
            }
        }
    } else {
        invalid("unsupported expression " + expression.kind);
    }
    return result;
}

std::vector<PlanColumn> schema(const catalog::Table& table) {
    std::vector<PlanColumn> output;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        output.push_back({table.columns[i].name, table.columns[i].type, i, table.columns[i].nullable, table.columns[i].defaultValue, table.columns[i].primaryKey, table.columns[i].unique, table.columns[i].references});
    }
    return output;
}

nlohmann::json expressionIdentity(nlohmann::json value) {
    if (!value.is_object()) return value;
    value.erase("line"); value.erase("column");
    if (value.contains("left")) value["left"] = expressionIdentity(value["left"]);
    if (value.contains("right")) value["right"] = expressionIdentity(value["right"]);
    return value;
}
void lowerAggregate(LogicalPlan& project, const Statement& statement, const catalog::Table& scope) {
    if (project.kind != "Project" || project.children.size() != 1) invalid("aggregate requires a projection input");
    LogicalPlan aggregate;
    aggregate.kind = "Aggregate";aggregate.table = project.table;
    std::unordered_map<std::string, std::size_t> groups, functions;
    for (const auto& key : statement.groupBy) {
        auto expression = bindExpression(*key, scope);
        const auto identity = expressionIdentity(expression).dump();
        if (groups.contains(identity)) continue;
        const auto index = aggregate.output.size();
        groups.emplace(identity, index);
        aggregate.output.push_back({"_group_" + std::to_string(index), expression.at("type").get<std::string>(), index, expression.value("nullable", true)});
        aggregate.groupKeys.push_back(std::move(expression));
    }
    const auto reference = [&](std::size_t index, const nlohmann::json& expression) {
        const auto& column = aggregate.output.at(index);
        return nlohmann::json{{"kind", "Identifier"}, {"columnId", index}, {"name", column.name},
            {"type", column.type}, {"nullable", column.nullable},
            {"line", expression.value("line", std::size_t{0})}, {"column", expression.value("column", std::size_t{0})}};
    };
    // 聚合以上的表达式只引用分组键或聚合槽位，不保留原始行列引用。
    std::function<nlohmann::json(nlohmann::json, std::size_t)> rewrite;
    rewrite = [&](nlohmann::json expression, std::size_t depth) -> nlohmann::json {
        if (depth > 256) invalid("aggregate expression depth exceeded");
        const auto identity = expressionIdentity(expression).dump();
        if (const auto group = groups.find(identity); group != groups.end()) return reference(group->second, expression);
        const auto kind = expression.at("kind").get<std::string>();
        if (kind == "AggregateExpr") {
            auto found = functions.find(identity);
            if (found == functions.end()) {
                const auto index = aggregate.output.size();
                found = functions.emplace(identity, index).first;
                aggregate.output.push_back({"_aggregate_" + std::to_string(index), expression.at("type").get<std::string>(), index, expression.value("nullable", true)});
                aggregate.aggregates.push_back({{"function", expression.at("function")}, {"argument", expression.at("left")},
                    {"columnId", index}, {"type", expression.at("type")}, {"nullable", expression.at("nullable")}});
            }
            return reference(found->second, expression);
        }
        if (kind == "Identifier") throw MiniSqlError(ErrorCode::Semantic, "Column must be grouped or aggregated: " + expression.at("name").get<std::string>(),
            {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
        if (expression.contains("left")) expression["left"] = rewrite(expression.at("left"), depth + 1);
        if (expression.contains("right")) expression["right"] = rewrite(expression.at("right"), depth + 1);
        return expression;
    };
    if (project.projections.empty()) for (const auto& column : project.output)
        project.projections.push_back(bindExpression(Expr{"Identifier", column.name, {}, {}, statement.location}, scope));
    for (std::size_t i = 0; i < project.projections.size(); ++i) {
        auto& expression = project.projections[i];
        expression = rewrite(expression, 0);
        project.output.at(i).columnId = expression.at("kind") == "Identifier" ? expression.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
    }
    auto having = statement.having ? rewrite(bindExpression(*statement.having, scope), 0) : nlohmann::json(nullptr);
    // X09 4.x: 聚合之上（HAVING / 投影 / ORDER BY）的相关子查询 —— 其外层列引用携带的是
    // 基表列下标，而此处实际求值的行是聚合输出行（分组键 + 聚合槽位）。必须把外层列下标
    // 重映射到 GROUP BY 键在 aggregate.output 中的槽位，否则执行期会越界或取自错误列。
    // 引用未参与分组的列按 SQL 语义报错（与 rewrite 对普通标识符的处理一致）。
    std::unordered_map<std::size_t, std::size_t> groupSlot;
    for (std::size_t i = 0; i < aggregate.groupKeys.size(); ++i) {
        const auto& key = aggregate.groupKeys[i];
        if (key.is_object() && key.value("kind", "") == "Identifier" && key.contains("columnId"))
            groupSlot.emplace(key.at("columnId").get<std::size_t>(), i);
    }
    std::function<void(nlohmann::json&)> remapGroupRefs;
    remapGroupRefs = [&](nlohmann::json& node) {
        if (!node.is_object()) return;
        const auto kind = node.value("kind", "");
        if ((kind == "ScalarSubquery" || kind == "Exists" || kind == "InSubquery") &&
            node.contains("subquerySql") && node.at("subquerySql").is_string() &&
            node.contains("outerColumns") && node.at("outerColumns").is_object()) {
            std::vector<std::string> referenced;
            try {
                const auto toks = tokenize(node.at("subquerySql").get<std::string>());
                for (std::size_t i = 0; i + 2 < toks.size(); ++i)
                    if (toks[i].type == "IDENTIFIER" && toks[i + 1].lexeme == "." && toks[i + 2].type == "IDENTIFIER")
                        referenced.push_back(canonical(toks[i].lexeme + "." + toks[i + 2].lexeme));
            } catch (...) { referenced.clear(); }
            std::map<std::string, std::size_t> slots;
            for (const auto& name : referenced) {
                const auto found = node.at("outerColumns").find(name);
                if (found == node.at("outerColumns").end() || !found->is_object() || !found->contains("columnId")) continue;
                const auto slot = groupSlot.find(found->at("columnId").get<std::size_t>());
                if (slot == groupSlot.end()) {
                    const auto dot = name.find('.');
                    throw MiniSqlError(ErrorCode::Semantic, "Column must be grouped or aggregated: " +
                        (dot == std::string::npos ? name : name.substr(dot + 1)), statement.location);
                }
                slots.emplace(name, slot->second);
            }
            for (const auto& entry : slots) node.at("outerColumns").at(entry.first)["columnId"] = entry.second;
        }
        if (node.contains("left")) remapGroupRefs(node["left"]);
        if (node.contains("right")) remapGroupRefs(node["right"]);
    };
    for (auto& expression : project.projections) remapGroupRefs(expression);
    if (!having.is_null()) remapGroupRefs(having);
    aggregate.children.push_back(std::move(project.children.front()));
    project.children.clear();
    if (statement.having) {
        LogicalPlan filter;
        filter.kind = "Filter";filter.table = aggregate.table;filter.output = aggregate.output;
        filter.predicate = std::move(having);filter.children.push_back(std::move(aggregate));
        project.children.push_back(std::move(filter));
    } else project.children.push_back(std::move(aggregate));
}
// X09 4.x: 把 WHERE 顶层 AND 中“可提升的相关子查询”（EXISTS/NOT EXISTS/IN）改写为
// SemiJoin/AntiSemiJoin 节点包裹 input，返回剩余（非子查询）谓词；无可提升时原样返回
// predicate。仅对单表、非聚合的 SELECT/DELETE/UPDATE 调用（见 build WHERE 分支）。
nlohmann::json decorrelateWhere(LogicalPlan& input, const nlohmann::json& predicate,
                                const catalog::Catalog& catalog);
LogicalPlan build(const Statement& statement, const catalog::Catalog& catalog) {
    if (statement.kind == "Begin" || statement.kind == "Commit" || statement.kind == "Rollback") {
        LogicalPlan plan;plan.kind = statement.kind;return plan;
    }
    if (statement.kind == "Checkpoint") { LogicalPlan plan; plan.kind = "Checkpoint"; return plan; }
    if (statement.kind == "DropIndex") { LogicalPlan plan; plan.kind = "DropIndex"; plan.table = statement.table; plan.indexName = statement.indexName; return plan; }
    if (statement.kind == "CreateIndex") { LogicalPlan plan; plan.kind = "CreateIndex"; plan.table = statement.table; plan.indexName = statement.indexName; plan.uniqueIndex = statement.uniqueIndex; plan.indexColumns = statement.indexColumns; return plan; }
    // X09 3.3: 派生表作为外层关系基座。内层 select 计划先构建，其实体输出
    // （selectItems）成为外层查询的绑定作用域；未限定列名一律按该作用域解析，禁字符串替换。
    const bool derivedBase = statement.fromSubquery != nullptr;
    const auto* table = derivedBase ? nullptr : catalog.find(statement.table);
    if (!table && !derivedBase) invalid("missing table " + statement.table);
    LogicalPlan plan;
    catalog::Table bindScope;
    LogicalPlan derivedInput;
    if (derivedBase) {
        derivedInput = build(*statement.fromSubquery, catalog);
        plan.table = statement.tableAlias.empty() ? statement.table : statement.tableAlias;
        catalog::Table derived;
        derived.name = plan.table;
        for (const auto& column : derivedInput.output)
            derived.columns.push_back({column.name, column.type, derived.name, column.nullable, column.defaultValue, column.primaryKey, column.unique, column.references});
        bindScope = std::move(derived);
    } else {
        plan.table = table->name;
        for (const auto& check : table->checks)
            plan.checks.push_back(bindExpression(*deserializeExpression(nlohmann::json::parse(check)), *table));
        bindScope = catalog::queryScope(statement, catalog);
    }
    const bool aggregated = statement.kind == "Select" && catalog::analyzeSelect(statement, bindScope).aggregated;
    if (statement.kind == "CreateTable") {
        plan.kind = "CreateTable";
        plan.checkDefinitions = serializeChecks(statement.checks);
        plan.keys = table->keys;
        plan.foreignKeys = table->foreignKeys;
        plan.constraintNames = table->constraintNames;
        plan.output = schema(*table);
    } else if (statement.kind == "Insert") {
        plan.kind = "Insert";
        if (!statement.valueRows.empty()) {
            auto single = statement;
            single.valueRows.clear();
            single.values.clear();
            for (const auto& row : statement.valueRows) {
                single.valueExpressions = row;
                const auto item = build(single, catalog);
                plan.insertRows.push_back({{"values", item.values}, {"expressions", item.insertExpressions}});
                plan.columnMapping = item.columnMapping;
            }
            return plan;
        }
        plan.values = nlohmann::json::array();
        if (!statement.valueExpressions.empty())
            for (std::size_t i = 0; i < table->columns.size(); ++i)
                plan.insertExpressions.push_back(bindExpression(Expr{"Literal", table->columns[i].defaultValue.value_or("NULL"), {}, {}, statement.location}, catalog::Table{"", {}}));
        for (const auto& column : table->columns) plan.values.push_back(literalValue(column.defaultValue.value_or("NULL")));
        const auto names = catalog::insertColumns(statement, *table);
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto index = columnIndex(*table, names[i]);
            plan.columnMapping.push_back(index);
            if (statement.valueExpressions.empty()) plan.values[index] = literalValue(statement.values[i]);
            else {
                const auto& expression = *statement.valueExpressions[i];
                if (expression.kind == "Default") {
                    const auto raw = table->columns[index].defaultValue.value_or("NULL");
                    plan.insertExpressions[index] = bindExpression(Expr{"Literal", raw, {}, {}, expression.location}, catalog::Table{"", {}});
                    plan.values[index] = literalValue(raw);
                } else plan.insertExpressions[index] = bindExpression(expression, catalog::Table{"", {}});
                if (expression.kind == "Literal") plan.values[index] = literalValue(expression.value);
            }
        }
    } else if (statement.kind == "Select" || statement.kind == "Delete" || statement.kind == "Update") {
        if (derivedBase && statement.kind != "Select")
            throw MiniSqlError(ErrorCode::Semantic, "DELETE/UPDATE is not supported over a derived table", statement.location);
        LogicalPlan input;
        if (derivedBase) {
            if (!statement.joins.empty())
                throw MiniSqlError(ErrorCode::Semantic, "JOIN over a derived table is not supported yet", statement.location);
            // 派生表基座：直接以内层 select 计划作为输入，外层谓词/投影按 derivedScope 绑定。
            input = std::move(derivedInput);
        } else {
        LogicalPlan scan;
        scan.kind = "SeqScan";
        scan.table = table->name;
        scan.output = schema(*table);
        scan.preservesRowId = true;
        if (statement.kind == "Select" && !aggregated && statement.joins.empty() && statement.where) {
            std::unordered_map<std::string, std::shared_ptr<Expr>> equalityLiterals;
            std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<Expr>>> rangeLiterals;
            std::function<void(const Expr&)> collect = [&](const Expr& expression) {
                if (expression.kind == "Binary" && expression.value == "AND" && expression.left && expression.right) {
                    collect(*expression.left);
                    collect(*expression.right);
                    return;
                }
                if (expression.kind != "Binary" || !expression.left || !expression.right ||
                    expression.left->kind != "Identifier" || expression.right->kind != "Literal") return;
                const auto name = canonical(expression.left->value);
                if (expression.value == "=") equalityLiterals[name] = expression.right;
                else if (expression.value == "<" || expression.value == "<=" || expression.value == ">" || expression.value == ">=")
                    rangeLiterals[name] = {expression.value, expression.right};
            };
            collect(*statement.where);
            for (const auto& index : table->indexes) {
                if (index.columns.empty()) continue;
                std::vector<std::shared_ptr<Expr>> values;
                std::size_t prefix = 0;
                for (; prefix < index.columns.size(); ++prefix) {
                    const auto found = equalityLiterals.find(canonical(index.columns[prefix]));
                    if (found == equalityLiterals.end()) break;
                    values.push_back(found->second);
                }
                bool selected = prefix == index.columns.size();
                std::pair<std::string, std::shared_ptr<Expr>> range;
                if (!selected && prefix < index.columns.size()) {
                    const auto found = rangeLiterals.find(canonical(index.columns[prefix]));
                    if (found != rangeLiterals.end()) { range = found->second; selected = true; }
                }
                if (!selected) continue;
                scan.kind = "IndexScan";
                scan.indexName = index.name;
                scan.indexColumns = index.columns;
                scan.indexValues = nlohmann::json::array();
                for (const auto& value : values) scan.indexValues.push_back(bindExpression(*value, bindScope));
                if (prefix < index.columns.size()) {
                    scan.indexRangeOperator = range.first;
                    scan.indexRangeValue = bindExpression(*range.second, bindScope);
                }
                break;
            }
        }
        input = std::move(scan);
        }
        for (std::size_t i = 0; i < statement.joins.size(); ++i) {
            const auto& source = statement.joins[i];
            const auto* right = catalog.find(source.table);
            if (!right) invalid("missing join table");
            LogicalPlan rightScan;
            rightScan.kind = "SeqScan";rightScan.table = right->name;
            rightScan.output = schema(*right);rightScan.preservesRowId = true;
            const auto prefix = catalog::queryScope(statement, catalog, i + 1);
            LogicalPlan join;
            join.kind = source.left && source.right ? "FullJoin" : source.left ? "LeftJoin" : source.right ? "RightJoin" : "NestedLoopJoin";join.table = table->name;
            join.output = schema(prefix);join.predicate = bindExpression(*source.on, prefix);
            join.children.push_back(std::move(input));join.children.push_back(std::move(rightScan));
            input = std::move(join);
        }
        if (statement.where) {
            auto wherePredicate = bindExpression(*statement.where, bindScope);
            const bool canDecorate = !aggregated && !derivedBase && statement.joins.empty() &&
                statement.kind == "Select";
            if (canDecorate) {
                // X09 4.x: 相关子查询去相关（WHERE 顶层 AND 的可提升 EXISTS/NOT EXISTS/IN）。
                const auto remainder = decorrelateWhere(input, wherePredicate, catalog);
                const bool trivial = remainder.is_object() && remainder.value("kind", "") == "Literal" &&
                    remainder.contains("value") && remainder.at("value").is_boolean() && remainder.at("value").get<bool>();
                if (!remainder.is_null() && !trivial) {
                    LogicalPlan filter;
                    filter.kind = "Filter";
                    filter.table = plan.table;
                    filter.output = input.output;
                    filter.preservesRowId = input.preservesRowId;
                    filter.predicate = remainder;
                    filter.children.push_back(std::move(input));
                    input = std::move(filter);
                }
            } else {
                LogicalPlan filter;
                filter.kind = "Filter";
                filter.table = plan.table;
                filter.output = input.output;
                filter.preservesRowId = input.preservesRowId;
                filter.predicate = std::move(wherePredicate);
                filter.children.push_back(std::move(input));
                input = std::move(filter);
            }
        }
        plan.kind = statement.kind == "Select" ? "Project" : statement.kind;
        if (statement.kind == "Update") {
            for (const auto& item : statement.assignments) {
                const auto index = columnIndex(*table, item.column);
                plan.columnMapping.push_back(index);
                if (item.expression->kind == "Default")
                    plan.projections.push_back(bindExpression(Expr{"Literal", table->columns[index].defaultValue.value_or("NULL"), {}, {}, item.expression->location}, bindScope));
                else plan.projections.push_back(bindExpression(*item.expression, bindScope));
            }
        }
        if (statement.kind == "Select") {
            if (!statement.selectItems.empty()) {
                for (std::size_t i = 0; i < statement.selectItems.size(); ++i) {
                    const auto& item = statement.selectItems[i];
                    if (item.expression->kind == "Wildcard") {
                        const auto dot = item.expression->value.find('.');
                        for (const auto& column : schema(bindScope)) {
                            const auto& source = bindScope.columns[column.columnId];
                            if (dot != std::string::npos && canonical(source.qualifier) != canonical(item.expression->value.substr(0, dot))) continue;
                            plan.output.push_back(column);
                            Expr reference{"Identifier", source.qualifier + "." + column.name, {}, {}, item.expression->location};
                            plan.projections.push_back(bindExpression(reference, bindScope));
                        }
                        continue;
                    }
                    auto bound = bindExpression(*item.expression, bindScope);
                    auto name = item.alias;
                    if (name.empty()) name = item.expression->kind == "Identifier" ? bound.at("name").get<std::string>() : "expr_" + std::to_string(i + 1);
                    const auto columnId = item.expression->kind == "Identifier" ? bound.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
                    plan.output.push_back({name, bound.at("type").get<std::string>(), columnId, bound.value("nullable", true)});
                    plan.projections.push_back(std::move(bound));
                }
            } else if (!derivedBase) for (const auto& name : statement.selectList) {
                if (name == "*") plan.output = schema(*table);
                else {
                    auto index = columnIndex(*table, name);
                    plan.output.push_back({table->columns[index].name, table->columns[index].type, index});
                }
            }
        }
        plan.children.push_back(std::move(input));
    } else {
        invalid("unsupported statement " + statement.kind);
    }
    nlohmann::json sortKeys = nlohmann::json::array();
    auto visibleOutput = plan.output;
    if (statement.kind == "Select" && !statement.orderBy.empty()) {
        if (plan.projections.empty() && !derivedBase) {
            for (const auto& column : plan.output) {
                Expr reference{"Identifier", column.name, {}, {}};
                plan.projections.push_back(bindExpression(reference, bindScope));
            }
        }
        for (const auto& item : statement.orderBy) {
            const auto resolved = catalog::resolveOrder(statement, item, bindScope);
            const auto bound = bindExpression(*resolved, bindScope);
            const auto identity = expressionIdentity(bound);
            std::size_t index = 0;
            while (index < plan.projections.size() && expressionIdentity(plan.projections[index]) != identity) ++index;
            if (index == plan.projections.size()) {
                if (statement.distinct) throw MiniSqlError(ErrorCode::Semantic, "DISTINCT ORDER BY must match a projected expression", item.expression->location);
                plan.projections.push_back(bound);
                plan.output.push_back({"_sort_" + std::to_string(sortKeys.size()), bound.at("type").get<std::string>(), static_cast<std::size_t>(-1), bound.value("nullable", true)});
            }
            sortKeys.push_back({{"index", index}, {"descending", item.descending}, {"nullsFirst", item.nullsFirst.value_or(item.descending)}});
        }
    }
    if (aggregated) {
        lowerAggregate(plan, statement, bindScope);
        std::copy_n(plan.output.begin(), visibleOutput.size(), visibleOutput.begin());
    }
    if (statement.kind == "Select" && statement.distinct) {
        LogicalPlan distinct;
        distinct.kind = "Distinct";
        distinct.table = plan.table;
        distinct.output = plan.output;
        distinct.children.push_back(std::move(plan));
        plan = std::move(distinct);
    }
    if (!sortKeys.empty()) {
        LogicalPlan sorted;
        sorted.kind = "Sort";
        sorted.table = plan.table;
        sorted.output = visibleOutput;
        sorted.sortKeys = std::move(sortKeys);
        sorted.children.push_back(std::move(plan));
        plan = std::move(sorted);
    }
    if (statement.kind == "Select" && (statement.limit || statement.offset)) {
        LogicalPlan limit;
        limit.kind = "Limit";
        limit.table = plan.table;
        limit.output = plan.output;
        limit.limit = statement.limit;
        limit.offset = statement.offset;
        limit.children.push_back(std::move(plan));
        plan = std::move(limit);
    }
    return plan;
}
nlohmann::json decorrelateWhere(LogicalPlan& input, const nlohmann::json& predicate,
                                const catalog::Catalog& catalog) {
    // 顶层 AND 拆分。
    std::vector<nlohmann::json> conjuncts;
    std::function<void(const nlohmann::json&)> split;
    split = [&](const nlohmann::json& node) {
        if (node.is_object() && node.value("kind", "") == "Binary" && node.value("operator", "") == "AND" &&
            node.contains("left") && node.contains("right")) {
            split(node.at("left"));
            split(node.at("right"));
        } else if (node.is_object()) conjuncts.push_back(node);
    };
    split(predicate);
    const auto isCorrelated = [](const nlohmann::json& node) -> bool {
        if (!node.is_object() || !node.contains("outerColumns") || !node.at("outerColumns").is_object()) return false;
        const auto& scope = node.at("outerColumns");
        const auto sql = node.value("subquerySql", std::string());
        if (sql.empty()) return false;
        try {
            const auto toks = tokenize(sql);
            for (std::size_t i = 0; i + 2 < toks.size(); ++i)
                if (toks[i].type == "IDENTIFIER" && toks[i + 1].lexeme == "." && toks[i + 2].type == "IDENTIFIER" &&
                    scope.contains(canonical(toks[i].lexeme + "." + toks[i + 2].lexeme))) return true;
        } catch (...) { return false; }
        return false;
    };
    // X09 4.x: in-process 子计划可执行性。执行器的 joinRows 只支持 Project/Filter/Scan/Join
    // 与 Apply/SemiJoin 本身；含 Aggregate/Sort/Limit/Distinct 的子计划只能在顶层 `run` 中
    // 执行。这类相关子查询退回执行期按行绑定（Correlated* 表达式）路径：语义一致，且该路径
    // 已在投影位置长期验证，避免生成运行期必然失败的 Apply/SemiJoin。
    std::function<bool(const LogicalPlan&)> inProcessExecutable;
    inProcessExecutable = [&](const LogicalPlan& node) -> bool {
        const auto supported = node.kind == "SemiJoin" || node.kind == "AntiSemiJoin" || node.kind == "Apply" ||
            node.kind == "Project" || node.kind == "Filter" || node.kind == "IndexScan" || node.kind == "SeqScan" ||
            node.kind == "NestedLoopJoin" || node.kind == "HashJoin" || node.kind == "LeftJoin" ||
            node.kind == "RightJoin" || node.kind == "FullJoin";
        if (!supported) return false;
        for (const auto& child : node.children) if (!inProcessExecutable(child)) return false;
        return true;
    };
    const auto buildRight = [&](const nlohmann::json& node, nlohmann::json& paramBindingOut,
                                bool inSubquery) -> std::pair<bool, LogicalPlan> {
        // 一次性（compiled-once）把子查询编译为结构化右子计划；外层列按 outerColumns
        // 序生成参量，落入活动关联作用域绑定为 Parameter。
        paramBindingOut = nlohmann::json::array();
        CorrelatedScope corr;
        std::size_t paramId = 0;
        const auto& scope = node.at("outerColumns");
        for (auto it = scope.begin(); it != scope.end(); ++it) {
            const auto type = it.value().at("type").get<std::string>();
            corr[canonical(it.key())] = {paramId, type};
            paramBindingOut.push_back({{"paramId", paramId},
                {"columnId", it.value().at("columnId").get<std::size_t>()}, {"type", type}});
            ++paramId;
        }
        std::vector<Statement> ast;
        try { ast = parse(tokenize(node.at("subquerySql").get<std::string>() + ";")); }
        catch (...) { return {false, {}}; }
        if (ast.size() != 1 || ast.front().kind != "Select") return {false, {}};
        try {
            CorrelatedScopeGuard guard(&corr);
            auto subplan = build(ast.front(), catalog);
            if (inSubquery && subplan.output.size() != 1) return {false, {}};
            if (!inProcessExecutable(subplan)) return {false, {}};
            return {true, std::move(subplan)};
        } catch (...) { return {false, {}}; }
    };
    nlohmann::json remaining = nullptr;
    const auto isScalarSub = [&](const nlohmann::json& n) -> bool {
        return n.is_object() && n.value("kind", "") == "ScalarSubquery" && isCorrelated(n);
    };
    for (const auto& conjunct : conjuncts) {
        const bool ifExists = conjunct.value("kind", "") == "Exists" && isCorrelated(conjunct);
        const bool ifNotExists = conjunct.value("kind", "") == "Unary" && conjunct.value("operator", "") == "NOT" &&
            conjunct.contains("left") && conjunct.at("left").is_object() &&
            conjunct.at("left").value("kind", "") == "Exists" && isCorrelated(conjunct.at("left"));
        const bool ifIn = conjunct.value("kind", "") == "InSubquery" && isCorrelated(conjunct);
        // X09 4.x: NOT IN 相关 → AntiSemiJoin(残差)；标量相关 → Apply(scalar)。
        const bool ifNotIn = conjunct.value("kind", "") == "Unary" && conjunct.value("operator", "") == "NOT" &&
            conjunct.contains("left") && conjunct.at("left").is_object() &&
            conjunct.at("left").value("kind", "") == "InSubquery" && isCorrelated(conjunct.at("left"));
        const bool ifScalarBool = isScalarSub(conjunct);
        const bool ifScalarComp = conjunct.value("kind", "") == "Binary" &&
            ((conjunct.contains("left") && isScalarSub(conjunct.at("left"))) ||
             (conjunct.contains("right") && isScalarSub(conjunct.at("right"))));
        if (ifExists || ifNotExists || ifIn || ifNotIn || ifScalarBool || ifScalarComp) {
            nlohmann::json target = conjunct;
            if (ifNotExists || ifNotIn) target = conjunct.at("left");
            else if (ifScalarBool) target = conjunct;
            else if (ifScalarComp) target = isScalarSub(conjunct.at("left")) ? conjunct.at("left") : conjunct.at("right");
            nlohmann::json paramBinding;
            auto [ok, subplan] = buildRight(target, paramBinding, ifIn || ifNotIn || ifScalarBool || ifScalarComp);
            nlohmann::json residual = nullptr;
            if (ok) {
                if (ifIn || ifNotIn) {
                    // IN/NOT IN 残差：左操作数与右子计划首列等值。
                    const nlohmann::json* inOperand = nullptr;
                    if (ifIn && conjunct.contains("left")) inOperand = &conjunct.at("left");
                    else if (ifNotIn && conjunct.at("left").contains("left")) inOperand = &conjunct.at("left").at("left");
                    if (inOperand) residual = nlohmann::json{{"kind", "Binary"}, {"operator", "="}, {"type", "bool"}, {"nullable", true},
                        {"left", *inOperand},
                        {"right", nlohmann::json{{"kind", "Identifier"}, {"columnId", input.output.size()},
                            {"name", subplan.output.front().name}, {"type", subplan.output.front().type}, {"nullable", true}}}};
                } else if (ifScalarBool || ifScalarComp) {
                    // 标量残差：追加列为右子计划首列；比较型则把子查询一侧替换为该列。
                    const auto appended = nlohmann::json{{"kind", "Identifier"}, {"columnId", input.output.size()},
                        {"name", subplan.output.front().name}, {"type", subplan.output.front().type}, {"nullable", true}};
                    if (ifScalarBool) residual = appended;
                    else {
                        // 把子查询一侧替换为追加标量列，另一侧保持原位：
                        //   left 为子查询 → left=appended,  right=conjunct.right
                        //   right 为子查询 → left=conjunct.left, right=appended
                        if (isScalarSub(conjunct.at("left")))
                            residual = nlohmann::json{{"kind", "Binary"}, {"operator", conjunct.value("operator", "=")},
                                {"type", "bool"}, {"nullable", true},
                                {"left", appended}, {"right", conjunct.at("right")}};
                        else
                            residual = nlohmann::json{{"kind", "Binary"}, {"operator", conjunct.value("operator", "=")},
                                {"type", "bool"}, {"nullable", true},
                                {"left", conjunct.at("left")}, {"right", appended}};
                    }
                }
            }
            if (ok) {
                LogicalPlan node;
                if (ifScalarBool || ifScalarComp) {
                    node.kind = "Apply";
                    node.values = nlohmann::json{{"scalar", true}};
                } else node.kind = (ifNotExists || ifNotIn) ? "AntiSemiJoin" : "SemiJoin";
                node.table = input.table;
                node.output = input.output;
                node.preservesRowId = input.preservesRowId;
                node.paramBinding = std::move(paramBinding);
                if (!residual.is_null()) node.predicate = std::move(residual);
                node.children.push_back(std::move(input));
                node.children.push_back(std::move(subplan));
                input = std::move(node);
                continue;
            }
        }
        if (remaining.is_null()) remaining = conjunct;
        else remaining = nlohmann::json{{"kind", "Binary"}, {"operator", "AND"}, {"type", "bool"}, {"nullable", true},
            {"left", std::move(remaining)}, {"right", conjunct}, {"line", conjunct.value("line", std::size_t{0})}};
    }
    return remaining;
}
}

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
                                      const catalog::Catalog& catalog) {
    auto snapshot = catalog;
    std::optional<catalog::Catalog> transactionCatalog;
    std::vector<LogicalPlan> plans;
    for (const auto& statement : statements) {
        if (statement.kind == "Begin") transactionCatalog = snapshot;
        else if (statement.kind == "Rollback" && transactionCatalog) { snapshot = *transactionCatalog;transactionCatalog.reset(); }
        else if (statement.kind == "Commit") transactionCatalog.reset();
        snapshot = catalog::compileSnapshot({statement}, snapshot);
        plans.push_back(build(statement, snapshot));
    }
    return plans;
}

nlohmann::json serializePlans(const std::vector<LogicalPlan>& plans) {
    nlohmann::json rows = nlohmann::json::array();
    std::size_t nextId = 0;
    std::function<std::size_t(const LogicalPlan&, int, std::size_t, std::size_t)> visit;
    visit = [&](const LogicalPlan& plan, int parent, std::size_t depth, std::size_t statementIndex) {
        const auto id = nextId++;
        const auto rowIndex = rows.size();
        nlohmann::json output = nlohmann::json::array();
        for (const auto& column : plan.output) {
            output.push_back({{"name", column.name}, {"type", column.type}, {"columnId", column.columnId}, {"nullable", column.nullable},
                {"defaultValue", column.defaultValue ? nlohmann::json(*column.defaultValue) : nlohmann::json(nullptr)}, {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", serializeReference(column.references)}});
        }
        rows.push_back({{"id", id}, {"parent", parent}, {"depth", depth}, {"statementIndex", statementIndex},
                        {"kind", plan.kind}, {"detail", plan.kind + " " + plan.table}, {"table", plan.table},
                        {"indexName", plan.indexName}, {"uniqueIndex", plan.uniqueIndex}, {"indexColumns", plan.indexColumns}, {"indexValues", plan.indexValues}, {"indexRangeOperator", plan.indexRangeOperator}, {"indexRangeValue", plan.indexRangeValue},
                        {"output", output}, {"preservesRowId", plan.preservesRowId},
                        {"predicate", plan.predicate}, {"values", plan.values}, {"insertExpressions", plan.insertExpressions}, {"insertRows", plan.insertRows},
                        {"columnMapping", plan.columnMapping}, {"projections", plan.projections}, {"paramBinding", plan.paramBinding}, {"children", nlohmann::json::array()}});
        rows[rowIndex]["limit"] = plan.limit ? nlohmann::json(std::to_string(*plan.limit)) : nlohmann::json(nullptr);
        rows[rowIndex]["offset"] = std::to_string(plan.offset);
        rows[rowIndex]["sortKeys"] = plan.sortKeys;
        rows[rowIndex]["groupKeys"] = plan.groupKeys;
        rows[rowIndex]["aggregates"] = plan.aggregates;
        rows[rowIndex]["keys"] = serializeKeys(plan.keys);
        rows[rowIndex]["checks"] = plan.checks;
        rows[rowIndex]["checkDefinitions"] = plan.checkDefinitions;
        rows[rowIndex]["foreignKeys"] = serializeForeignKeys(plan.foreignKeys);
        rows[rowIndex]["constraintNames"] = serializeConstraintNames(plan.constraintNames);
        for (const auto& child : plan.children) {
            const auto childId = visit(child, static_cast<int>(id), depth + 1, statementIndex);
            rows[rowIndex]["children"].push_back(childId);
        }
        return id;
    };
    for (std::size_t i = 0; i < plans.size(); ++i) visit(plans[i], -1, 0, i);
    return rows;
}

std::vector<LogicalPlan> deserializePlans(const nlohmann::json& document) {
    const auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized logical plan"); };
    nlohmann::json rows;
    if (document.is_array()) rows = document;
    else if (document.is_object() && document.value("schemaVersion", 0u) == PLAN_SCHEMA_VERSION && document.value("planKind", "") == "logical" && document.contains("plans"))
        rows = document.at("plans");
    else invalid();
    if (!rows.is_array() || rows.size() > 65536) invalid();

    struct Pending {
        LogicalPlan plan;
        std::int64_t parent = -1;
        std::size_t statementIndex = 0;
        std::vector<std::size_t> children;
    };
    std::vector<Pending> pending;
    pending.reserve(rows.size());
    std::function<void(const nlohmann::json&, std::size_t)> validateExpression;
    validateExpression = [&](const nlohmann::json& expression, std::size_t depth) {
        if (depth > 256 || !expression.is_object()) invalid();
        const auto kind = expression.value("kind", "");
        if (kind != "Literal" && kind != "Identifier" && kind != "Cast" &&
            kind != "Unary" && kind != "Binary" && kind != "AggregateExpr" && kind != "Parameter") invalid();
        if (expression.contains("left") && !expression.at("left").is_null()) validateExpression(expression.at("left"), depth + 1);
        if (expression.contains("right") && !expression.at("right").is_null()) validateExpression(expression.at("right"), depth + 1);
    };

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows.at(index);
        if (!row.is_object() || !row.contains("id") || !row.at("id").is_number_unsigned() || row.at("id").get<std::size_t>() != index ||
            !row.contains("parent") || !row.at("parent").is_number_integer() || !row.contains("depth") || !row.at("depth").is_number_unsigned() ||
            !row.contains("statementIndex") || !row.at("statementIndex").is_number_unsigned() || !row.contains("kind") || !row.at("kind").is_string() ||
            !row.contains("table") || !row.at("table").is_string() || !row.contains("output") || !row.at("output").is_array() ||
            !row.contains("children") || !row.at("children").is_array() || !row.contains("columnMapping") || !row.at("columnMapping").is_array() ||
            !row.contains("preservesRowId") || !row.at("preservesRowId").is_boolean()) invalid();
        Pending item;
        item.parent = row.at("parent").get<std::int64_t>();
        if (item.parent < -1 || item.parent >= static_cast<std::int64_t>(rows.size())) invalid();
        item.statementIndex = row.at("statementIndex").get<std::size_t>();
        item.plan.kind = row.at("kind").get<std::string>();
        item.plan.table = row.at("table").get<std::string>();
        item.plan.preservesRowId = row.at("preservesRowId").get<bool>();
        item.plan.indexName = row.value("indexName", std::string{});
        item.plan.uniqueIndex = row.value("uniqueIndex", false);
        item.plan.indexColumns = row.value("indexColumns", std::vector<std::string>{});
        item.plan.indexValues = row.value("indexValues", nlohmann::json::array());
        item.plan.indexRangeOperator = row.value("indexRangeOperator", std::string{});
        item.plan.indexRangeValue = row.value("indexRangeValue", nlohmann::json(nullptr));
        item.plan.paramBinding = row.value("paramBinding", nlohmann::json::array());
        if (!item.plan.paramBinding.is_array()) invalid();
        for (const auto& column : row.at("output")) {
            if (!column.is_object() || !column.contains("name") || !column.at("name").is_string() || !column.contains("type") || !column.at("type").is_string() ||
                !column.contains("columnId") || !column.at("columnId").is_number_unsigned() || !column.contains("nullable") || !column.at("nullable").is_boolean()) invalid();
            PlanColumn value{column.at("name").get<std::string>(), column.at("type").get<std::string>(), column.at("columnId").get<std::size_t>(), column.at("nullable").get<bool>()};
            if (column.contains("defaultValue") && !column.at("defaultValue").is_null()) value.defaultValue = column.at("defaultValue").get<std::string>();
            value.primaryKey = column.value("primaryKey", false);
            value.unique = column.value("unique", false);
            if (column.contains("references") && !column.at("references").is_null()) {
                if (!column.at("references").is_object()) invalid();
                value.references = std::make_pair(column.at("references").at("table").get<std::string>(), column.at("references").at("column").get<std::string>());
            }
            item.plan.output.push_back(std::move(value));
        }
        for (const auto& child : row.at("children")) {
            if (!child.is_number_unsigned() || child.get<std::size_t>() >= rows.size()) invalid();
            item.children.push_back(child.get<std::size_t>());
        }
        for (const auto& value : row.at("columnMapping")) {
            if (!value.is_number_unsigned()) invalid();
            item.plan.columnMapping.push_back(value.get<std::size_t>());
        }
        if (row.contains("limit") && !row.at("limit").is_null()) {
            if (!row.at("limit").is_string()) invalid();
            std::uint64_t limit{};
            const auto text = row.at("limit").get<std::string>();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
            item.plan.limit = limit;
        }
        if (row.contains("offset")) {
            if (!row.at("offset").is_string()) invalid();
            const auto text = row.at("offset").get<std::string>();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), item.plan.offset);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
        }
        item.plan.predicate = row.value("predicate", nlohmann::json(nullptr));
        item.plan.values = row.value("values", nlohmann::json::array());
        item.plan.insertExpressions = row.value("insertExpressions", nlohmann::json::array());
        item.plan.insertRows = row.value("insertRows", nlohmann::json::array());
        item.plan.projections = row.value("projections", nlohmann::json::array());
        item.plan.sortKeys = row.value("sortKeys", nlohmann::json::array());
        item.plan.groupKeys = row.value("groupKeys", nlohmann::json::array());
        item.plan.aggregates = row.value("aggregates", nlohmann::json::array());
        item.plan.checks = row.value("checks", nlohmann::json::array());
        item.plan.checkDefinitions = row.value("checkDefinitions", nlohmann::json::array());
        if (!item.plan.predicate.is_null()) validateExpression(item.plan.predicate, 0);
        for (const auto& expression : item.plan.projections) validateExpression(expression, 0);
        for (const auto& expression : item.plan.groupKeys) validateExpression(expression, 0);
        for (const auto& aggregate : item.plan.aggregates) {
            if (!aggregate.is_object() || !aggregate.contains("argument")) invalid();
            if (!aggregate.at("argument").is_null()) validateExpression(aggregate.at("argument"), 0);
        }
        for (const auto& expression : item.plan.insertExpressions) if (!expression.is_null()) validateExpression(expression, 0);
        for (const auto& inserted : item.plan.insertRows) {
            if (!inserted.is_object() || !inserted.contains("expressions")) invalid();
            for (const auto& expression : inserted.at("expressions")) if (!expression.is_null()) validateExpression(expression, 0);
        }
        if (row.contains("keys")) for (const auto& key : row.at("keys")) item.plan.keys.push_back({key.at("primary").get<bool>(), key.at("columns").get<std::vector<std::string>>()});
        if (row.contains("foreignKeys")) for (const auto& key : row.at("foreignKeys")) item.plan.foreignKeys.push_back({key.at("columns").get<std::vector<std::string>>(), key.at("table").get<std::string>(), key.at("referencedColumns").get<std::vector<std::string>>()});
        if (row.contains("constraintNames")) for (const auto& binding : row.at("constraintNames")) item.plan.constraintNames.push_back({binding.at("name").get<std::string>(), binding.at("kind").get<std::string>(), binding.at("index").get<std::size_t>()});
        pending.push_back(std::move(item));
    }
    std::vector<bool> attached(rows.size(), false);
    std::function<LogicalPlan(std::size_t, std::size_t)> buildTree;
    buildTree = [&](std::size_t index, std::size_t depth) -> LogicalPlan {
        if (depth > 256 || index >= pending.size() || attached[index]) invalid();
        attached[index] = true;
        auto plan = pending[index].plan;
        for (const auto child : pending[index].children) {
            if (pending[child].parent != static_cast<std::int64_t>(index)) invalid();
            plan.children.push_back(buildTree(child, depth + 1));
        }
        return plan;
    };
    std::vector<LogicalPlan> result;
    for (std::size_t index = 0; index < pending.size(); ++index) if (pending[index].parent == -1) result.push_back(buildTree(index, 0));
    if (std::any_of(attached.begin(), attached.end(), [](bool value) { return !value; })) invalid();
    return result;
}
}
