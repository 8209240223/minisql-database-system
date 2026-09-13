#include "minisql/sql/planner.hpp"
#include "minisql/sql/serialization.hpp"
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

// X25：planner 的扁平作用域槽位 -> 绑定器的稳定标识。
// 槽位顺序（FROM 项 + 各 JOIN 依次拼接）与绑定器的关系顺序一致，但这里不依赖
// 该巧合：每个槽位按 (qualifier, name) 在作用域树里查，查不到就留 0（无身份），
// 下游据此退回槽位语义，不会因为绑定缺失而算错。
struct SlotIdentity { std::uint32_t binding = 0; std::uint32_t relation = 0; };
using SlotMap = std::vector<SlotIdentity>;

SlotMap slotIdentities(const catalog::Table& scope, const BindResult& bound, ScopeId scopeId) {
    SlotMap slots(scope.columns.size());
    if (!validId(scopeId)) return slots;
    for (std::size_t index = 0; index < scope.columns.size(); ++index) {
        const auto& column = scope.columns[index];
        const auto resolved = bound.scopes.resolveColumn(column.qualifier, column.name, scopeId);
        if (!resolved) continue;
        slots[index] = {rawId(resolved->column), rawId(resolved->relation)};
    }
    return slots;
}

SlotIdentity slotAt(const SlotMap& slots, std::size_t index) {
    return index < slots.size() ? slots[index] : SlotIdentity{};
}

// 表达式绑定需要的上下文：槽位->身份映射，以及绑定结果本身（用于相关子查询）。
struct BindContext {
    const BindResult* bound = nullptr;
    SlotMap slots{};
};

// 子节点（基表扫描、JOIN 右侧）的槽位是外层槽位的一个连续窗口：
// 取出该窗口，子节点的局部槽位就能拿到同一份稳定身份。
SlotMap slotWindow(const SlotMap& slots, std::size_t offset, std::size_t count) {
    SlotMap window(count);
    for (std::size_t index = 0; index < count && offset + index < slots.size(); ++index)
        window[index] = slots[offset + index];
    return window;
}

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

// 外层作用域快照。键仍是 "qualifier.name"——执行器 (database.cpp) 目前按这个
// 键把内层 AST 的标识符文本绑到外层行槽位上。值里额外带上 binding/relation，
// 执行器迁移到按身份绑定之后，字符串键即可退役。
nlohmann::json correlatedScope(const catalog::Table& table, const SlotMap& slots) {
    nlohmann::json scope = nlohmann::json::object();
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        const auto& column = table.columns[index];
        if (column.qualifier.empty()) continue;
        const auto identity = slotAt(slots, index);
        scope[canonical(column.qualifier + "." + column.name)] =
            {{"columnId", index}, {"type", column.type},
             {"binding", identity.binding}, {"relation", identity.relation}};
    }
    return scope;
}

// 子查询实际引用到的外层列，来自绑定器而不是对 subquerySql 的再扫描。
// 每项给出稳定身份与它在外层行里的槽位，供聚合下降时重映射。
nlohmann::json outerReferences(const BindContext& context, const Expr& node) {
    auto result = nlohmann::json::array();
    if (!context.bound || !context.bound->complete) return result;
    for (const auto& reference : context.bound->correlatedFor(&node)) {
        const auto* column = context.bound->scopes.column(reference.column);
        if (!column) continue;
        std::size_t slot = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < context.slots.size(); ++index)
            if (context.slots[index].binding == rawId(reference.column)) { slot = index; break; }
        result.push_back({{"binding", rawId(reference.column)}, {"relation", rawId(reference.relation)},
                          {"name", column->name}, {"columnId", slot},
                          {"correlationDepth", reference.correlationDepth}});
    }
    return result;
}
bool containsSubquery(const Expr& expression) {
    if (expression.kind == "Exists" || expression.kind == "InSubquery" || expression.kind == "ScalarSubquery") return true;
    return (expression.left && containsSubquery(*expression.left)) || (expression.right && containsSubquery(*expression.right));
}
bool containsNegatedSubquery(const Expr& expression) {
    if (expression.kind == "Unary" && canonical(expression.value) == "not" && expression.left && containsSubquery(*expression.left)) return true;
    return (expression.left && containsNegatedSubquery(*expression.left)) || (expression.right && containsNegatedSubquery(*expression.right));
}
nlohmann::json bindExpression(const Expr& expression, const catalog::Table& table,
                              const BindContext& context = {}, std::size_t depth = 0) {
    if (depth > 256) invalid("expression depth exceeded");
    nlohmann::json result = {{"kind", expression.kind}, {"line", expression.location.line},
                             {"column", expression.location.column}};
    if (context.bound) result["expressionId"] = rawId(context.bound->idFor(&expression));
    if (expression.kind == "Identifier") {
        const auto index = columnIndex(table, expression.value);
        // columnId 是运行时槽位；binding/relation 是稳定身份。两者分开之后，
        // 裁剪与下推可以看身份，执行器继续看槽位。
        result["columnId"] = index;
        const auto identity = slotAt(context.slots, index);
        result["binding"] = identity.binding;
        result["relation"] = identity.relation;
        result["name"] = table.columns[index].name;
        result["type"] = table.columns[index].type;
        result["nullable"] = table.columns[index].nullable;
    } else if (expression.kind == "Literal") {
        result["value"] = literalValue(expression.value);
        result["type"] = result["value"].is_null() ? "null" : result["value"].is_boolean() ? "bool" : result["value"].is_string() ? "varchar" : result["value"].is_number_float() ? "float" : "int";
        if (result["value"].is_number_integer() && (result["value"].get<std::int64_t>() < INT32_MIN || result["value"].get<std::int64_t>() > INT32_MAX)) result["type"] = "bigint";
        result["nullable"] = result["value"].is_null();
        const bool dateLiteral = dateLiteralText(expression.value).has_value();
        const auto firstNumeric = [&]() {
            if (expression.value.empty()) return false;
            const std::size_t offset = expression.value.front() == '+' || expression.value.front() == '-' ? 1 : 0;
            return offset < expression.value.size() && std::isdigit(static_cast<unsigned char>(expression.value[offset]));
        };
        if (dateLiteral) result["type"] = "date";
        if (!dateLiteral && firstNumeric() && expression.value.find_first_of("eE") != std::string::npos) result["type"] = "float";
        if (firstNumeric() && expression.value.find_first_of("eE") == std::string::npos && expression.value.find('.') != std::string::npos) result["type"] = decimalLiteral(expression.value, expression.location).type.name();
    } else if (expression.kind == "Exists") {
        if (expression.subquerySql.empty()) invalid("missing EXISTS subquery");
        result["subquerySql"] = expression.subquerySql;
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "SemiJoin" : "Apply";
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        result["type"] = "bool";
        result["nullable"] = false;
    } else if (expression.kind == "ScalarSubquery") {
        if (expression.subquerySql.empty()) invalid("missing scalar subquery");
        result["subquerySql"] = expression.subquerySql;
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "ScalarSubquery" : "Apply";
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        result["type"] = "null";
        result["nullable"] = true;
    } else if (expression.kind == "InSubquery") {
        if (!expression.left || expression.subquerySql.empty()) invalid("missing IN subquery operand");
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        result["subquerySql"] = expression.subquerySql;
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "SemiJoin" : "Apply";
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        result["type"] = "bool";
        result["nullable"] = true;
    } else if (expression.kind == "AggregateExpr") {
        if (!expression.left) invalid("missing aggregate argument");
        result["function"] = expression.value;
        result["left"] = expression.left->kind == "Wildcard" ? nlohmann::json(nullptr) : bindExpression(*expression.left, table, context, depth + 1);
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
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        result["nullable"] = result["left"].value("nullable", true);
    } else if (expression.kind == "Unary" || expression.kind == "Binary") {
        result["operator"] = expression.value;
        result["type"] = isArithmetic(expression.value) ? "int" : "bool";
        if (!expression.left) invalid("missing left operand");
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        if (expression.kind == "Binary") {
            if (!expression.right) invalid("missing right operand");
            result["right"] = bindExpression(*expression.right, table, context, depth + 1);
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

std::vector<PlanColumn> schema(const catalog::Table& table, const SlotMap& slots = {}) {
    std::vector<PlanColumn> output;
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        const auto identity = slotAt(slots, i);
        output.push_back({table.columns[i].name, table.columns[i].type, i, table.columns[i].nullable, table.columns[i].defaultValue,
            table.columns[i].primaryKey, table.columns[i].unique, table.columns[i].references,
            identity.binding, identity.relation});
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
void lowerAggregate(LogicalPlan& project, const Statement& statement, const catalog::Table& scope,
                    const BindContext& context) {
    if (project.kind != "Project" || project.children.size() != 1) invalid("aggregate requires a projection input");
    LogicalPlan aggregate;
    aggregate.kind = "Aggregate";aggregate.table = project.table;
    std::unordered_map<std::string, std::size_t> groups, functions;
    for (const auto& key : statement.groupBy) {
        auto expression = bindExpression(*key, scope, context);
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
        project.projections.push_back(bindExpression(Expr{"Identifier", column.name, {}, {}, statement.location}, scope, context));
    for (std::size_t i = 0; i < project.projections.size(); ++i) {
        auto& expression = project.projections[i];
        expression = rewrite(expression, 0);
        project.output.at(i).columnId = expression.at("kind") == "Identifier" ? expression.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
    }
    auto having = statement.having ? rewrite(bindExpression(*statement.having, scope, context), 0) : nlohmann::json(nullptr);
    // X09 4.x: 聚合之上（HAVING / 投影）的相关子查询 —— 其 outerColumns 携带的是基表列下标，
    // 而此处实际求值的行是聚合输出行（分组键 + 聚合槽位）。必须把外层列下标重映射到 GROUP BY
    // 键在 aggregate.output 中的槽位，否则执行期会越界（5001）或取自错误列。
    // 引用未参与分组的列按 SQL 语义报 2003，与 rewrite 对普通标识符的处理一致。
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
            std::map<std::string, std::size_t> slots;
            if (node.contains("outerReferences") && node.at("outerReferences").is_array()) {
                // 绑定器已经精确给出这个子查询引用到的外层列。此前这里是把
                // subquerySql 重新分词、按 `IDENT . IDENT` 模式猜引用——未限定的
                // 外层引用猜不到，字符串里的同名片段又会误命中。
                for (auto& reference : node.at("outerReferences")) {
                    if (!reference.is_object() || !reference.contains("columnId")) continue;
                    const auto outerSlot = reference.at("columnId").get<std::size_t>();
                    const auto grouped = groupSlot.find(outerSlot);
                    if (grouped == groupSlot.end())
                        throw MiniSqlError(ErrorCode::Semantic, "Column must be grouped or aggregated: " +
                            reference.value("name", std::string{}), statement.location);
                    const auto binding = reference.value("binding", std::uint32_t{0});
                    for (auto entry : node.at("outerColumns").items())
                        if (entry.value().is_object() && entry.value().value("binding", std::uint32_t{0}) == binding)
                            slots.emplace(entry.key(), grouped->second);
                    reference["columnId"] = grouped->second;
                }
            } else {
                // 没有绑定信息（旧计划文档）时退回原来的词法扫描，行为不变。
                std::vector<std::string> referenced;
                try {
                    const auto toks = tokenize(node.at("subquerySql").get<std::string>());
                    for (std::size_t i = 0; i + 2 < toks.size(); ++i)
                        if (toks[i].type == "IDENTIFIER" && toks[i + 1].lexeme == "." && toks[i + 2].type == "IDENTIFIER")
                            referenced.push_back(canonical(toks[i].lexeme + "." + toks[i + 2].lexeme));
                } catch (...) { referenced.clear(); }
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
LogicalPlan build(const Statement& statement, const catalog::Catalog& catalog, const BindResult& bound) {
    if (statement.kind == "Begin" || statement.kind == "Commit" || statement.kind == "Rollback" ||
        statement.kind == "Savepoint" || statement.kind == "ReleaseSavepoint" || statement.kind == "RollbackTo") {
        LogicalPlan plan;plan.kind = statement.kind;plan.savepointName = statement.savepointName;return plan;
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
        derivedInput = build(*statement.fromSubquery, catalog, bound);
        plan.table = statement.tableAlias.empty() ? statement.table : statement.tableAlias;
        catalog::Table derived;
        derived.name = plan.table;
        for (const auto& column : derivedInput.output)
            derived.columns.push_back({column.name, column.type, derived.name, column.nullable, column.defaultValue, column.primaryKey, column.unique, column.references});
        bindScope = std::move(derived);
        for (const auto& join : statement.joins) {
            const auto* right = catalog.find(join.table);
            if (!right) invalid("missing join table");
            const auto qualifier = join.alias.empty() ? right->name : join.alias;
            if (join.right) for (auto& column : bindScope.columns) column.nullable = true;
            for (auto column : right->columns) { column.qualifier = qualifier; if (join.left) column.nullable = true; bindScope.columns.push_back(std::move(column)); }
        }
    } else {
        plan.table = table->name;
        for (const auto& check : table->checks)
            plan.checks.push_back(bindExpression(*deserializeExpression(nlohmann::json::parse(check)), *table));
        bindScope = catalog::queryScope(statement, catalog);
    }
    // 该语句在绑定结果里的作用域；绑定失败或语句不在结果中时 slots 全 0，
    // 计划仍按槽位语义构建，只是没有稳定身份。
    const auto* boundStatement = bound.statementFor(&statement);
    const auto slots = slotIdentities(bindScope, bound, boundStatement ? boundStatement->scope : ScopeId::Invalid);
    const BindContext context{&bound, slots};
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
                const auto item = build(single, catalog, bound);
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
        LogicalPlan input;
        if (derivedBase) {
            if (statement.kind == "Select") input = std::move(derivedInput);
            else {
                const auto* physical = catalog.find(statement.fromSubquery->table);
                if (!physical || derivedInput.kind != "Project" || derivedInput.children.size() != 1)
                    invalid("derived table is not updatable");
                plan.table = physical->name;
                table = physical;
                input = std::move(derivedInput.children.front());
            }
        } else {
        LogicalPlan scan;
        scan.kind = "SeqScan";
        scan.table = table->name;
        scan.output = schema(*table, slotWindow(slots, 0, table->columns.size()));
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
                for (const auto& value : values) scan.indexValues.push_back(bindExpression(*value, bindScope, context));
                if (prefix < index.columns.size()) {
                    scan.indexRangeOperator = range.first;
                    scan.indexRangeValue = bindExpression(*range.second, bindScope, context);
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
            catalog::Table prefix;
            const auto rightOffset = input.output.size();
            prefix.name = bindScope.name;
            prefix.columns.assign(bindScope.columns.begin(), bindScope.columns.begin() + static_cast<std::ptrdiff_t>(rightOffset + right->columns.size()));
            LogicalPlan rightScan;
            rightScan.kind = "SeqScan";rightScan.table = right->name;
            rightScan.output = schema(*right, slotWindow(slots, rightOffset, right->columns.size()));
            rightScan.preservesRowId = true;
            LogicalPlan join;
            join.kind = source.left && source.right ? "FullJoin" : source.left ? "LeftJoin" : source.right ? "RightJoin" : "NestedLoopJoin";join.table = plan.table;
            join.output = input.output;
            if (source.right) for (auto& column : join.output) column.nullable = true;
            auto rightOutput = rightScan.output;
            if (source.left) for (auto& column : rightOutput) column.nullable = true;
            join.output.insert(join.output.end(), rightOutput.begin(), rightOutput.end());
            join.predicate = bindExpression(*source.on, prefix, context);
            join.children.push_back(std::move(input));join.children.push_back(std::move(rightScan));
            input = std::move(join);
        }
        if (statement.where) {
            LogicalPlan filter;
            filter.kind = containsNegatedSubquery(*statement.where) ? "AntiJoin" :
                containsSubquery(*statement.where) ? "SemiJoin" : "Filter";
            filter.subqueryJoinKind = containsNegatedSubquery(*statement.where) ? "AntiJoin" :
                containsSubquery(*statement.where) ? "SemiJoin" : "";
            filter.table = plan.table;
            filter.output = input.output;
            filter.preservesRowId = input.preservesRowId;
            filter.predicate = bindExpression(*statement.where, bindScope, context);
            filter.children.push_back(std::move(input));
            input = std::move(filter);
        }
        plan.kind = statement.kind == "Select" ? "Project" : statement.kind;
        if (statement.kind == "Update") {
            for (const auto& item : statement.assignments) {
                const auto index = columnIndex(*table, item.column);
                plan.columnMapping.push_back(index);
                if (item.expression->kind == "Default")
                    plan.projections.push_back(bindExpression(Expr{"Literal", table->columns[index].defaultValue.value_or("NULL"), {}, {}, item.expression->location}, bindScope, context));
                else plan.projections.push_back(bindExpression(*item.expression, bindScope, context));
            }
        }
        if (statement.kind == "Select") {
            if (!statement.selectItems.empty()) {
                for (std::size_t i = 0; i < statement.selectItems.size(); ++i) {
                    const auto& item = statement.selectItems[i];
                    if (item.expression->kind == "Wildcard") {
                        const auto dot = item.expression->value.find('.');
                        for (const auto& column : schema(bindScope, slots)) {
                            const auto& source = bindScope.columns[column.columnId];
                            if (dot != std::string::npos && canonical(source.qualifier) != canonical(item.expression->value.substr(0, dot))) continue;
                            plan.output.push_back(column);
                            Expr reference{"Identifier", source.qualifier + "." + column.name, {}, {}, item.expression->location};
                            plan.projections.push_back(bindExpression(reference, bindScope, context));
                        }
                        continue;
                    }
                    auto bound = bindExpression(*item.expression, bindScope, context);
                    auto name = item.alias;
                    if (name.empty()) name = item.expression->kind == "Identifier" ? bound.at("name").get<std::string>() : "expr_" + std::to_string(i + 1);
                    const auto columnId = item.expression->kind == "Identifier" ? bound.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
                    PlanColumn projected{name, bound.at("type").get<std::string>(), columnId, bound.value("nullable", true)};
                    // 投影列继承被投影标识符的稳定身份；表达式列没有身份（0）。
                    projected.binding = bound.value("binding", std::uint32_t{0});
                    projected.relation = bound.value("relation", std::uint32_t{0});
                    projected.expression = bound.value("expressionId", std::uint32_t{0});
                    plan.output.push_back(std::move(projected));
                    plan.projections.push_back(std::move(bound));
                }
            } else if (!derivedBase) for (const auto& name : statement.selectList) {
                if (name == "*") plan.output = schema(*table, slots);
                else {
                    auto index = columnIndex(*table, name);
                    const auto identity = slotAt(slots, index);
                    PlanColumn column{table->columns[index].name, table->columns[index].type, index};
                    column.binding = identity.binding;
                    column.relation = identity.relation;
                    plan.output.push_back(std::move(column));
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
                plan.projections.push_back(bindExpression(reference, bindScope, context));
            }
        }
        for (const auto& item : statement.orderBy) {
            const auto resolved = catalog::resolveOrder(statement, item, bindScope);
            const auto bound = bindExpression(*resolved, bindScope, context);
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
        lowerAggregate(plan, statement, bindScope, context);
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
}

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
                                      const catalog::Catalog& catalog) {
    auto snapshot = catalog;
    std::optional<catalog::Catalog> transactionCatalog;
    std::map<std::string, catalog::Catalog> savepointCatalogs;
    std::vector<LogicalPlan> plans;
    for (const auto& statement : statements) {
        if (statement.kind == "Begin") { transactionCatalog = snapshot; savepointCatalogs.clear(); }
        else if (statement.kind == "Savepoint" && transactionCatalog) savepointCatalogs[canonical(statement.savepointName)] = snapshot;
        else if (statement.kind == "ReleaseSavepoint") savepointCatalogs.erase(canonical(statement.savepointName));
        else if (statement.kind == "RollbackTo" && transactionCatalog) {
            const auto found = savepointCatalogs.find(canonical(statement.savepointName));
            if (found == savepointCatalogs.end()) invalid("savepoint does not exist: " + statement.savepointName);
            snapshot = found->second;
        }
        else if (statement.kind == "Rollback" && transactionCatalog) { snapshot = *transactionCatalog;transactionCatalog.reset();savepointCatalogs.clear(); }
        else if (statement.kind == "Commit") { transactionCatalog.reset();savepointCatalogs.clear(); }
        snapshot = catalog::compileSnapshot({statement}, snapshot);
        {
            // 逐句绑定：批内 DDL 会改变 Catalog 快照，所以不能对整批只绑一次。
            // 传指针而非拷贝，保证 BindResult 里的裸指针指向调用方持有的语句。
            const auto bound = bindStatements({&statement}, snapshot);
            auto built = build(statement, snapshot, bound);
            // 计划绑定编译时的 Catalog 指纹，供执行阶段检测 schema 失效。
            const auto fingerprint = snapshot.schemaFingerprint();
            std::function<void(LogicalPlan&)> stamp = [&](LogicalPlan& node) {
                node.catalogFingerprint = fingerprint;
                if (node.sourceSpan.line == 0) node.sourceSpan = statement.location;
                for (auto& child : node.children) stamp(child);
            };
            stamp(built);
            plans.push_back(std::move(built));
        }
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
            output.push_back({{"name", column.name}, {"type", column.type}, {"columnId", column.columnId}, {"binding", column.binding}, {"relation", column.relation}, {"expressionId", column.expression}, {"nullable", column.nullable},
                {"defaultValue", column.defaultValue ? nlohmann::json(*column.defaultValue) : nlohmann::json(nullptr)}, {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", serializeReference(column.references)}});
        }
        const auto endLine = plan.sourceSpan.endLine ? plan.sourceSpan.endLine : plan.sourceSpan.line;
        const auto endColumn = plan.sourceSpan.endColumn ? plan.sourceSpan.endColumn : plan.sourceSpan.column + (plan.sourceSpan.line ? 1 : 0);
        rows.push_back({{"id", id}, {"nodeId", id}, {"parent", parent}, {"depth", depth}, {"statementIndex", statementIndex},
                        {"kind", plan.kind}, {"detail", plan.kind + " " + plan.table}, {"table", plan.table},
                        {"sourceSpan", {{"start", {{"line", plan.sourceSpan.line}, {"column", plan.sourceSpan.column}}},
                                        {"end", {{"line", endLine}, {"column", endColumn}}}}},
                        {"catalogFingerprint", plan.catalogFingerprint},
                        {"optimizerDecision", plan.optimizerDecision},
                        {"indexName", plan.indexName}, {"savepointName", plan.savepointName}, {"subqueryJoinKind", plan.subqueryJoinKind}, {"uniqueIndex", plan.uniqueIndex}, {"indexColumns", plan.indexColumns}, {"indexValues", plan.indexValues}, {"indexRangeOperator", plan.indexRangeOperator}, {"indexRangeValue", plan.indexRangeValue},
                        {"output", output}, {"outputSchema", output}, {"preservesRowId", plan.preservesRowId},
                        {"predicate", plan.predicate}, {"values", plan.values}, {"insertExpressions", plan.insertExpressions}, {"insertRows", plan.insertRows},
                        {"columnMapping", plan.columnMapping}, {"projections", plan.projections}, {"children", nlohmann::json::array()}});
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
            kind != "Unary" && kind != "Binary" && kind != "AggregateExpr") invalid();
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
        item.plan.optimizerDecision = row.value("optimizerDecision", nlohmann::json(nullptr));
        if (row.contains("sourceSpan")) {
            const auto& span = row.at("sourceSpan");
            if (!span.is_object() || !span.contains("start") || !span.contains("end") ||
                !span.at("start").is_object() || !span.at("end").is_object()) invalid();
            item.plan.sourceSpan = {span.at("start").value("line", std::size_t{0}), span.at("start").value("column", std::size_t{0}),
                                    span.at("end").value("line", std::size_t{0}), span.at("end").value("column", std::size_t{0})};
        }
    item.plan.indexName = row.value("indexName", std::string{});
    item.plan.savepointName = row.value("savepointName", std::string{});
    item.plan.subqueryJoinKind = row.value("subqueryJoinKind", std::string{});        item.plan.catalogFingerprint = row.value("catalogFingerprint", std::string{});        item.plan.uniqueIndex = row.value("uniqueIndex", false);
        item.plan.indexColumns = row.value("indexColumns", std::vector<std::string>{});
        item.plan.indexValues = row.value("indexValues", nlohmann::json::array());
        item.plan.indexRangeOperator = row.value("indexRangeOperator", std::string{});
        item.plan.indexRangeValue = row.value("indexRangeValue", nlohmann::json(nullptr));
        for (const auto& column : row.at("output")) {
            if (!column.is_object() || !column.contains("name") || !column.at("name").is_string() || !column.contains("type") || !column.at("type").is_string() ||
                !column.contains("columnId") || !column.at("columnId").is_number_unsigned() || !column.contains("nullable") || !column.at("nullable").is_boolean()) invalid();
            PlanColumn value{column.at("name").get<std::string>(), column.at("type").get<std::string>(), column.at("columnId").get<std::size_t>(), column.at("nullable").get<bool>()};
            // X25：同 major 内的宽松读取——旧文档没有 binding/relation，缺失即无身份。
            value.binding = column.value("binding", std::uint32_t{0});
            value.relation = column.value("relation", std::uint32_t{0});
            value.expression = column.value("expressionId", std::uint32_t{0});
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
