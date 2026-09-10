#include "minisql/optimizer/optimizer.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/decimal.hpp"
#include <set>

namespace minisql::optimizer {
namespace {
using json = nlohmann::json;
bool literal(const json& expr) { return expr.is_object() && expr.value("kind", "") == "Literal"; }
bool boolean(const json& expr) { return literal(expr) && expr.contains("value") && expr.at("value").is_boolean(); }
ExactDecimal decimalConstant(const json& expression) {
    const auto type = decimalType(expression.at("type").get<std::string>());
    return type ? ExactDecimal::parse(expression.at("value").get<std::string>(), type->precision, type->scale) : ExactDecimal::fromInteger(expression.at("value").get<std::int64_t>());
}
json constant(const json& original, json value) {
    return {{"kind", "Literal"}, {"type", "bool"}, {"value", value},
            {"line", original.value("line", 0)}, {"column", original.value("column", 0)}};
}
void record(json& changes, const char* rule, std::size_t statement, const json& before, const json& after) {
    changes.push_back({{"ruleId", rule}, {"statementIndex", statement}, {"before", before}, {"after", after}});
}
json rewrite(json expression, const Options& options, json& changes, std::size_t statement, std::size_t depth = 0) {
    if (depth > 256) throw MiniSqlError(ErrorCode::Internal, "Optimizer expression depth exceeded");
    const auto kind = expression.value("kind", "");
    if (kind == "Literal" || kind == "Identifier" || kind == "Exists" || kind == "ScalarSubquery") return expression;
    if (kind == "InSubquery") {
        expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
        return expression;
    }
    if (kind == "Cast") {
        expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
        return expression;
    }
    if (kind != "Unary" && kind != "Binary") throw MiniSqlError(ErrorCode::Internal, "Unsupported optimizer expression");
    expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
    if (kind == "Binary") expression["right"] = rewrite(expression.at("right"), options, changes, statement, depth + 1);
    const auto op = expression.at("operator").get<std::string>();
    const auto& left = expression.at("left");
    auto replace = [&](json after, const char* rule) {
        if (after.value("type", "") == "null" && expression.at("type") == "bool") after["type"] = "bool";
        if (expression.at("type") != after.at("type"))
            throw MiniSqlError(ErrorCode::Internal, "Optimizer rewrite changed expression type");
        record(changes, rule, statement, expression, after);
        return after;
    };
    if (options.constantArithmetic && isArithmetic(op) && decimalType(expression.at("type").get<std::string>()) &&
        literal(left) && !left.at("value").is_null() && (kind == "Unary" || (literal(expression.at("right")) && !expression.at("right").at("value").is_null()))) {
        try {
            const auto a = decimalConstant(left);
            const auto value = kind == "Unary" ? (op == "-" ? a.negated() : a) : a.arithmetic(op, decimalConstant(expression.at("right")));
            auto folded = expression;
            folded["kind"] = "Literal";folded["value"] = value.format();
            folded.erase("left");folded.erase("right");folded.erase("operator");
            return replace(folded, "constant-arithmetic");
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::Execution) throw;
        }
    }
    if (options.constantArithmetic && isArithmetic(op) && literal(left) && left.at("value").is_number_integer() &&
        (kind == "Unary" || (literal(expression.at("right")) && expression.at("right").at("value").is_number_integer()))) {
        try {
            auto a = left.at("value").get<std::int64_t>();
            auto b = kind == "Unary" ? a : expression.at("right").at("value").get<std::int64_t>();
            if (kind == "Unary") a = 0;
            auto folded = constant(expression, false);
            folded["type"] = expression.at("type");
            folded["value"] = expression.at("type") == "bigint" ? json(arithmetic64(op, a, b)) : json(arithmetic(op, static_cast<std::int32_t>(a), static_cast<std::int32_t>(b)));
            return replace(folded, "constant-arithmetic");
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::Execution) throw;
            // 保留可能抛错的子树，运行时由短路规则决定是否求值。
        }
    }
    if (options.booleanSimplification && literal(left)) {
        const auto& value = left.at("value");
        if (op == "IS NULL" || op == "IS NOT NULL")
            return replace(constant(expression, op == "IS NULL" ? value.is_null() : !value.is_null()), "boolean-simplification");
        if (op == "NOT" && (value.is_boolean() || value.is_null()))
            return replace(constant(expression, value.is_null() ? json(nullptr) : json(!value.get<bool>())), "boolean-simplification");
    }
    if (kind != "Binary") return expression;
    const auto& right = expression.at("right");
    if (options.booleanSimplification && (op == "AND" || op == "OR")) {
        // 仅同时为常量时使用三值真值表，避免 NULL 吸收规则跳过右侧异常。
        if (literal(left) && literal(right)) {
            const auto& a = left.at("value");
            const auto& b = right.at("value");
            if ((a.is_boolean() || a.is_null()) && (b.is_boolean() || b.is_null())) {
                const bool absorbing = op == "OR";
                const json value = a == absorbing || b == absorbing ? json(absorbing) :
                    a.is_null() || b.is_null() ? json(nullptr) : json(!absorbing);
                return replace(constant(expression, value), "boolean-simplification");
            }
        }
        if (boolean(left)) {
            const bool value = left.at("value").get<bool>();
            if ((op == "AND" && !value) || (op == "OR" && value))
                return replace(constant(expression, value), "boolean-simplification");
            return replace(right, "boolean-simplification");
        }
        // 只消去右侧恒等值，保留左侧求值，避免以后加入算术错误时被错误隐藏。
        if (boolean(right) && ((op == "AND" && right.at("value").get<bool>()) ||
                              (op == "OR" && !right.at("value").get<bool>())))
            return replace(left, "boolean-simplification");
    }
    if (options.constantComparison && literal(left) && literal(right)) {
        const auto& a = left.at("value");
        const auto& b = right.at("value");
        if (a.is_null() || b.is_null()) {
            if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
                return replace(constant(expression, nullptr), "constant-comparison");
            return expression;
        }
        bool value;
        if (decimalType(left.at("type").get<std::string>()) || decimalType(right.at("type").get<std::string>())) {
            const auto order = decimalConstant(left).compare(decimalConstant(right));
            if (op == "=") value = order == 0;
            else if (op == "!=") value = order != 0;
            else if (op == "<") value = order < 0;
            else if (op == "<=") value = order <= 0;
            else if (op == ">") value = order > 0;
            else if (op == ">=") value = order >= 0;
            else return expression;
        }
        else if (op == "=") value = a == b;
        else if (op == "!=") value = a != b;
        else if (op == "<") value = a < b;
        else if (op == "<=") value = a <= b;
        else if (op == ">") value = a > b;
        else if (op == ">=") value = a >= b;
        else return expression;
        return replace(constant(expression, value), "constant-comparison");
    }
    return expression;
}
bool safePushdownExpression(const json& expression, std::size_t depth = 0) {
    if (!expression.is_object() || depth > 64) return false;
    const auto kind = expression.value("kind", "");
    if (kind == "Literal" || kind == "Identifier") return true;
    if (kind == "Unary") {
        const auto op = expression.value("operator", "");
        if (op != "NOT" && op != "IS NULL" && op != "IS NOT NULL") return false;
        return expression.contains("left") && safePushdownExpression(expression.at("left"), depth + 1);
    }
    if (kind != "Binary") return false;
    const auto op = expression.value("operator", "");
    if (op != "AND" && op != "OR" && op != "=" && op != "!=" && op != "<" && op != "<=" && op != ">" && op != ">=") return false;
    return expression.contains("left") && expression.contains("right") &&
        safePushdownExpression(expression.at("left"), depth + 1) && safePushdownExpression(expression.at("right"), depth + 1);
}
void collectColumns(const json& expression, std::set<std::size_t>& columns, std::size_t depth = 0) {
    if (!expression.is_object() || depth > 64) return;
    if (expression.value("kind", "") == "Identifier" && expression.contains("columnId"))
        columns.insert(expression.at("columnId").get<std::size_t>());
    if (expression.contains("left")) collectColumns(expression.at("left"), columns, depth + 1);
    if (expression.contains("right")) collectColumns(expression.at("right"), columns, depth + 1);
}
json shiftColumns(json expression, std::size_t offset, std::size_t depth = 0) {
    if (!expression.is_object() || depth > 64) return expression;
    if (expression.value("kind", "") == "Identifier" && expression.contains("columnId")) {
        const auto id = expression.at("columnId").get<std::size_t>();
        if (id < offset) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown column shift underflow");
        expression["columnId"] = id - offset;
    }
    if (expression.contains("left")) expression["left"] = shiftColumns(expression.at("left"), offset, depth + 1);
    if (expression.contains("right")) expression["right"] = shiftColumns(expression.at("right"), offset, depth + 1);
    return expression;
}
void splitConjuncts(json expression, std::vector<json>& terms, std::size_t depth = 0) {
    if (depth > 64) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown depth exceeded");
    if (expression.is_object() && expression.value("kind", "") == "Binary" && expression.value("operator", "") == "AND") {
        splitConjuncts(expression.at("left"), terms, depth + 1);
        splitConjuncts(expression.at("right"), terms, depth + 1);
    } else terms.push_back(std::move(expression));
}
json combineConjuncts(std::vector<json> terms) {
    if (terms.empty()) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown produced empty predicate");
    auto result = std::move(terms.front());
    for (std::size_t i = 1; i < terms.size(); ++i) {
        const auto line = result.value("line", 0);
        const auto column = result.value("column", 0);
        result = {{"kind", "Binary"}, {"operator", "AND"}, {"type", "bool"}, {"nullable", true},
            {"left", std::move(result)}, {"right", std::move(terms[i])},
            {"line", line}, {"column", column}};
    }
    return result;
}
bool columnId(const json& expression, std::size_t& id) {
    if (!expression.is_object() || expression.value("kind", "") != "Identifier" || !expression.contains("columnId")) return false;
    id = expression.at("columnId").get<std::size_t>();
    return true;
}
bool hashJoinKeys(const json& predicate, std::size_t leftSize, std::size_t& leftKey, std::size_t& rightKey) {
    if (!predicate.is_object() || predicate.value("kind", "") != "Binary" || predicate.value("operator", "") != "=") return false;
    std::size_t a{}, b{};
    if (!columnId(predicate.at("left"), a) || !columnId(predicate.at("right"), b)) return false;
    if (a < leftSize && b >= leftSize) { leftKey = a; rightKey = b - leftSize; return true; }
    if (b < leftSize && a >= leftSize) { leftKey = b; rightKey = a - leftSize; return true; }
    return false;
}
bool pruneProjectColumns(sql::LogicalPlan& project, json& changes, std::size_t statement) {
    if (project.kind != "Project" || project.children.size() != 1 || project.projections.empty()) return false;
    auto& child = project.children.front();
    sql::LogicalPlan* filter = nullptr;
    sql::LogicalPlan* scan = nullptr;
    if (child.kind == "SeqScan") scan = &child;
    else if (child.kind == "Filter" && child.children.size() == 1 && child.children.front().kind == "SeqScan") {
        filter = &child;
        scan = &child.children.front();
    } else return false;
    std::set<std::size_t> required;
    for (const auto& expression : project.projections) collectColumns(expression, required);
    if (filter) collectColumns(filter->predicate, required);
    std::vector<sql::PlanColumn> pruned;
    for (const auto& column : scan->output) if (required.contains(column.columnId)) pruned.push_back(column);
    if (pruned.size() == scan->output.size()) return false;
    const auto before = sql::serializePlans({project});
    scan->output = std::move(pruned);
    record(changes, "prune-columns", statement, before, sql::serializePlans({project}));
    return true;
}
bool pushPredicateIntoJoin(sql::LogicalPlan& filter, json& changes, std::size_t statement) {
    if (filter.kind != "Filter" || filter.children.size() != 1 || (filter.children.front().kind != "NestedLoopJoin" && filter.children.front().kind != "HashJoin")) return false;
    auto joined = std::move(filter.children.front());
    auto& join = joined;
    if (join.children.size() != 2 || !safePushdownExpression(filter.predicate)) {
        filter.children.front() = std::move(joined);
        return false;
    }
    const auto leftSize = join.children[0].output.size();
    std::vector<json> terms, leftTerms, rightTerms, remaining;
    splitConjuncts(filter.predicate, terms);
    for (auto& term : terms) {
        if (!safePushdownExpression(term)) { remaining.push_back(std::move(term)); continue; }
        std::set<std::size_t> columns;
        collectColumns(term, columns);
        if (columns.empty()) { remaining.push_back(std::move(term)); continue; }
        const bool leftOnly = std::all_of(columns.begin(), columns.end(), [&](std::size_t id) { return id < leftSize; });
        const bool rightOnly = std::all_of(columns.begin(), columns.end(), [&](std::size_t id) { return id >= leftSize; });
        if (leftOnly) leftTerms.push_back(std::move(term));
        else if (rightOnly) rightTerms.push_back(shiftColumns(std::move(term), leftSize));
        else remaining.push_back(std::move(term));
    }
    if (leftTerms.empty() && rightTerms.empty()) return false;
    const auto before = sql::serializePlans({filter});
    const auto wrap = [](sql::LogicalPlan child, std::vector<json> predicates) {
        sql::LogicalPlan pushed;
        pushed.kind = "Filter";
        pushed.table = child.table;
        pushed.output = child.output;
        pushed.preservesRowId = child.preservesRowId;
        pushed.predicate = combineConjuncts(std::move(predicates));
        pushed.children.push_back(std::move(child));
        return pushed;
    };
    if (!leftTerms.empty()) join.children[0] = wrap(std::move(join.children[0]), std::move(leftTerms));
    if (!rightTerms.empty()) join.children[1] = wrap(std::move(join.children[1]), std::move(rightTerms));
    if (remaining.empty()) filter = std::move(joined);
    else {
        filter.children.front() = std::move(joined);
        filter.predicate = combineConjuncts(std::move(remaining));
    }
    record(changes, "predicate-pushdown", statement, before, sql::serializePlans({filter}));
    return true;
}
// X18 4.2: 优化器侧确定性成本估算。仅依据计划结构 + 可选的表行数回调，
// 同输入恒得同结果，用于候选执行策略的成本比较与固定决胜。
constexpr double kOptimizerDefaultRows = 1000.0;
constexpr double kOptimizerDefaultFilterSelectivity = 0.25;
double optimizerRows(const sql::LogicalPlan& plan, const Options& options) {
    if (plan.kind == "SeqScan")
        return options.tableRows ? options.tableRows(plan.table).value_or(kOptimizerDefaultRows) : kOptimizerDefaultRows;
    if (plan.children.empty()) {
        if (plan.kind == "Insert") return static_cast<double>(std::max(plan.values.size(), plan.insertRows.size()));
        return 1.0;
    }
    const auto rows = optimizerRows(plan.children.front(), options);
    if (plan.kind == "Filter")
        return rows * (options.selectivity ? options.selectivity(plan.predicate, plan.table) : kOptimizerDefaultFilterSelectivity);
    if (plan.kind == "Limit") return plan.limit ? std::min(rows, static_cast<double>(*plan.limit)) : rows;
    if (plan.kind == "Distinct") return rows * 0.5;
    if (plan.kind == "Aggregate") return plan.groupKeys.empty() ? 1.0 : std::min(rows * 0.1, kOptimizerDefaultRows);
    if (plan.kind == "NestedLoopJoin" || plan.kind == "HashJoin" || plan.kind == "LeftJoin" || plan.kind == "RightJoin" || plan.kind == "FullJoin") {
        if (plan.children.size() != 2) return rows;
        return rows * optimizerRows(plan.children[1], options) * 0.1;
    }
    return rows;  // Project / Sort 行数不变
}
void rewritePlan(sql::LogicalPlan& plan, const Options& options, json& changes, std::size_t statement, std::size_t depth = 0, bool selectQuery = false) {
    if (depth > 256) throw MiniSqlError(ErrorCode::Internal, "Optimizer plan depth exceeded");
    const bool childSelectQuery = selectQuery || plan.kind == "Project";
    for (auto& child : plan.children) rewritePlan(child, options, changes, statement, depth + 1, childSelectQuery);
    for (auto& expression : plan.projections) expression = rewrite(expression, options, changes, statement);
    for (auto& expression : plan.groupKeys) expression = rewrite(expression, options, changes, statement);
    for (auto& aggregate : plan.aggregates)
        if (!aggregate.at("argument").is_null()) aggregate["argument"] = rewrite(aggregate.at("argument"), options, changes, statement);
    for (auto& expression : plan.insertExpressions) expression = rewrite(expression, options, changes, statement);
    for (auto& row : plan.insertRows)
        for (auto& expression : row.at("expressions")) expression = rewrite(expression, options, changes, statement);
    if (plan.kind == "Project" && options.pruneColumns && pruneProjectColumns(plan, changes, statement)) return;
    if (plan.kind != "Filter" && plan.kind != "NestedLoopJoin" && plan.kind != "LeftJoin" && plan.kind != "HashJoin") return;
    plan.predicate = rewrite(plan.predicate, options, changes, statement);
    if (plan.kind == "NestedLoopJoin" && options.hashJoin && plan.children.size() == 2) {
        std::size_t leftKey{}, rightKey{};
        if (hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey)) {
            // X18 4.2: 成本驱动候选选择 + 固定决胜——Hash 成本 L+M，NestedLoop 成本 L*M；
            // 相等时不依赖容器序、固定偏好 HashJoin（确定性）。
            const auto leftRows = optimizerRows(plan.children[0], options);
            const auto rightRows = optimizerRows(plan.children[1], options);
            if (leftRows + rightRows <= leftRows * rightRows) {
                record(changes, "hash-join", statement, {{"kind", "NestedLoopJoin"}}, {{"kind", "HashJoin"}});
                plan.kind = "HashJoin";
            }
        }
    }
    if (plan.kind != "Filter") return;
    if (options.predicatePushdown && pushPredicateIntoJoin(plan, changes, statement)) {
        // X18 4.2-iv: 旁路下推后 plan 已「原地」成为 Join（Filter 之上被压平）。子节点此前
        // 在未带 Filter 时已按原始行数判过 Hash/NL（可能已改写为 HashJoin）；此刻 Filter
        // 已下推到 join 子输入，按带过滤的真实子输入（列级直方图选择率）双向重判 Hash/NL。
        if ((plan.kind == "NestedLoopJoin" || plan.kind == "HashJoin") && options.hashJoin && plan.children.size() == 2) {
            std::size_t leftKey{}, rightKey{};
            if (hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey)) {
                const auto leftRows = optimizerRows(plan.children[0], options);
                const auto rightRows = optimizerRows(plan.children[1], options);
                if (leftRows + rightRows <= leftRows * rightRows) {
                    if (plan.kind != "HashJoin") {
                        record(changes, "hash-join", statement, {{"kind", "NestedLoopJoin"}}, {{"kind", "HashJoin"}});
                        plan.kind = "HashJoin";
                    }
                } else if (plan.kind != "NestedLoopJoin") {
                    // 过滤后单侧大幅收窄，Hash(11) > NL(10) → 由先前判定（原始行数）的
                    // HashJoin 撤销为 NestedLoopJoin（确定性、成本驱动）。
                    record(changes, "hash-join", statement, {{"kind", "HashJoin"}}, {{"kind", "NestedLoopJoin"}});
                    plan.kind = "NestedLoopJoin";
                }
            }
        }
        return;
    }
    if (options.removeTrueFilter && boolean(plan.predicate) && plan.predicate.at("value").get<bool>() && plan.children.size() == 1) {
        const auto& input = plan.children.front();
        if (plan.table != input.table || plan.preservesRowId != input.preservesRowId || plan.output.size() != input.output.size()) return;
        for (std::size_t i = 0; i < plan.output.size(); ++i) {
            if (plan.output[i].name != input.output[i].name || plan.output[i].type != input.output[i].type ||
                plan.output[i].columnId != input.output[i].columnId || plan.output[i].nullable != input.output[i].nullable ||
                plan.output[i].defaultValue != input.output[i].defaultValue ||
                plan.output[i].primaryKey != input.output[i].primaryKey || plan.output[i].unique != input.output[i].unique ||
                plan.output[i].references != input.output[i].references) return;
        }
        record(changes, "remove-true-filter", statement, {{"kind", "Filter"}}, {{"kind", plan.children.front().kind}});
        auto child = std::move(plan.children.front());
        plan = std::move(child);
    }
    if (options.removeFalseFilter && selectQuery && literal(plan.predicate) && (plan.predicate.at("value") == false || plan.predicate.at("value").is_null()) && plan.children.size() == 1) {
        record(changes, "remove-false-filter", statement, {{"kind", "Filter"}, {"predicate", plan.predicate}}, {{"kind", "Limit"}, {"limit", "0"}});
        sql::LogicalPlan limit;
        limit.kind = "Limit";
        limit.table = plan.table;
        limit.output = plan.output;
        limit.limit = 0;
        limit.offset = 0;
        limit.children = std::move(plan.children);
        plan = std::move(limit);
    }
}
void checkBudget(const std::vector<sql::LogicalPlan>& plans, std::size_t maximum) {
    std::size_t nodes = 0;
    std::vector<std::pair<const sql::LogicalPlan*, std::size_t>> pending;
    for (const auto& plan : plans) pending.emplace_back(&plan, 0);
    auto countJson = [&](const json& root) {
        std::vector<std::pair<const json*, std::size_t>> values{{&root, 0}};
        while (!values.empty()) {
            const auto [value, depth] = values.back(); values.pop_back();
            if (++nodes > maximum || depth > 512)
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer node/depth budget exceeded");
            if (value->is_structured()) for (const auto& child : *value) values.emplace_back(&child, depth + 1);
        }
    };
    while (!pending.empty()) {
        const auto [plan, depth] = pending.back(); pending.pop_back();
        if (++nodes > maximum || depth > 256 || plan->output.size() > maximum - nodes)
            throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer node/depth budget exceeded");
        nodes += plan->output.size();
        for (const auto& key : plan->keys) {
            if (++nodes > maximum || key.columns.size() > maximum - nodes)
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer key budget exceeded");
            nodes += key.columns.size();
        }
        countJson(plan->predicate); countJson(plan->projections); countJson(plan->values); countJson(plan->sortKeys); countJson(plan->insertExpressions); countJson(plan->insertRows);
        countJson(plan->checks); countJson(plan->checkDefinitions);
        countJson(plan->groupKeys); countJson(plan->aggregates);
        if (plan->constraintNames.size() > (maximum - nodes) / 3)
            throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer constraint name budget exceeded");
        nodes += plan->constraintNames.size() * 3;
        for (const auto& reference : plan->foreignKeys) {
            if (++nodes > maximum || reference.columns.size() > maximum - nodes)
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer foreign key budget exceeded");
            nodes += reference.columns.size();
            if (reference.referencedColumns.size() > maximum - nodes)
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer foreign key budget exceeded");
            nodes += reference.referencedColumns.size();
        }
        for (const auto& child : plan->children) pending.emplace_back(&child, depth + 1);
    }
}
}
nlohmann::json ruleDescriptors() {
    return json::array({
        {{"ruleId", "constant-arithmetic"}, {"scope", "expression"}, {"precondition", "INT32 constant operands; successful checked arithmetic"}, {"postcondition", "Same INT32 value; errors remain unevaluated"}},
        {{"ruleId", "constant-comparison"}, {"scope", "expression"}, {"precondition", "Semantically bound constant operands of compatible types, including NULL"}, {"postcondition", "Same BOOL comparison result, including UNKNOWN"}},
        {{"ruleId", "boolean-simplification"}, {"scope", "expression"}, {"precondition", "BOOL/NULL constants or literal null tests; preserve left-to-right short circuit and possible errors"}, {"postcondition", "Same three-valued BOOL result and evaluation errors"}},
        {{"ruleId", "remove-true-filter"}, {"scope", "plan"}, {"precondition", "One child, same table/schema/RowId contract and literal BOOL true predicate"}, {"postcondition", "Preserve child schema, order, duplicates and RowId"}},
        {{"ruleId", "remove-false-filter"}, {"scope", "plan"}, {"precondition", "One child and literal FALSE or NULL predicate"}, {"postcondition", "Return no rows with unchanged output schema and do not execute child"}},
        {{"ruleId", "predicate-pushdown"}, {"scope", "plan"}, {"precondition", "INNER join Filter with side-local, side-effect-free comparison/boolean predicates"}, {"postcondition", "Same rows, NULL results and error timing for safe predicates"}},
        {{"ruleId", "hash-join"}, {"scope", "plan"}, {"precondition", "INNER NestedLoopJoin with direct left/right column equality"}, {"postcondition", "Same inner-join rows, duplicates and NULL non-matching semantics"}},
        {{"ruleId", "prune-columns"}, {"scope", "plan"}, {"precondition", "Project over SeqScan with optional single Filter; projections are explicit"}, {"postcondition", "Scan output metadata contains exactly referenced columns; row values and errors unchanged"}}
    });
}
Result optimize(const std::vector<sql::LogicalPlan>& plans, Options options) {
    if (options.maxIterations == 0 || options.maxIterations > 64 || options.maxNodes == 0 || options.maxNodes > 1000000)
        throw MiniSqlError(ErrorCode::InvalidArgument, "Invalid optimizer iteration/node budget");
    for (const auto& id : options.disabledRules) {
        if (id == "constant-arithmetic") options.constantArithmetic = false;
        else if (id == "constant-comparison") options.constantComparison = false;
        else if (id == "boolean-simplification") options.booleanSimplification = false;
        else if (id == "remove-true-filter") options.removeTrueFilter = false;
        else if (id == "remove-false-filter") options.removeFalseFilter = false;
        else if (id == "predicate-pushdown") options.predicatePushdown = false;
        else if (id == "hash-join") options.hashJoin = false;
        else if (id == "prune-columns") options.pruneColumns = false;
        else throw MiniSqlError(ErrorCode::InvalidArgument, "Unknown optimizer rule: " + id);
    }
    checkBudget(plans, options.maxNodes);
    Result result{plans, json::array()};
    auto previous = sql::serializePlans(result.plans).dump();
    std::set<std::string> seen{previous};
    for (std::size_t iteration = 1; iteration <= options.maxIterations; ++iteration) {
        const auto start = result.changes.size();
        for (std::size_t i = 0; i < result.plans.size(); ++i) rewritePlan(result.plans[i], options, result.changes, i);
        checkBudget(result.plans, options.maxNodes);
        result.iterations = iteration;
        for (std::size_t i = start; i < result.changes.size(); ++i) {
            result.changes[i]["iteration"] = iteration;
            result.changes[i]["sequence"] = i;
        }
        const auto current = sql::serializePlans(result.plans).dump();
        if (current == previous) { result.converged = true; return result; }
        if (!seen.insert(current).second) {
            result.diagnostics.push_back({{"code", "OPTIMIZER_CYCLE"}, {"message", "Repeated plan structure; optimization stopped"}});
            return result;
        }
        previous = current;
    }
    result.diagnostics.push_back({{"code", "OPTIMIZER_ITERATION_LIMIT"}, {"message", "Iteration budget reached; convergence not proven"}});
    return result;
}
}
