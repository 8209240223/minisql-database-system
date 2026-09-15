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
// 把名字统一转小写，作为大小写不敏感比较的规范形式。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    // 逐字符处理。
        return static_cast<char>(std::tolower(c));
        // 转小写；入参用无符号字符，避免负值传入造成未定义行为。
    });
    // 转换结束。
    return value;
    // 返回规范形式。
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
// 统一的"计划不变量被破坏"出口：这类问题说明编译器自身出错，而不是用户写错。
    throw MiniSqlError(ErrorCode::Internal, "Plan invariant: " + message);
    // 用 Internal 错误码抛出，并加上前缀方便定位。
}

std::size_t columnIndex(const catalog::Table& table, const std::string& name) {
// 把列名解析成列下标，转调目录层实现，保证两处解析规则完全一致。
    return catalog::resolveColumnIndex(table, name);
    // 复用目录层的解析（含大小写不敏感与歧义检测）。
}

nlohmann::json literalValue(const std::string& raw) {
// 把字面量的原始文本转成 JSON 值，供计划里的 values 字段使用。
    if (const auto date = dateLiteralText(raw)) return formatIsoDate(parseIsoDate(*date));
    // 日期字面量：先解析再按 ISO 格式规范化输出，保证表示唯一。
    if (canonical(raw) == "null") return nullptr;
    // NULL 对应 JSON 的 null。
    if (canonical(raw) == "true" || canonical(raw) == "false") return canonical(raw) == "true";
    // 布尔字面量对应 JSON 的 true/false。
    if (!raw.empty() && raw.front() == '\'') {
    // 以单引号开头的是字符串字面量。
        return stringLiteralValue(raw);
        // 交给公共工具脱引号并处理转义，得到真正的字符串内容。
    }
    // 字符串分支结束。
    if (raw.find_first_of("eE") != std::string::npos) return parseFiniteFloat(raw, ErrorCode::Internal);
    // 含 e/E 的按浮点解析；失败按内部错误抛出（这份文本应已在语法阶段校验过）。
    if (raw.find('.') != std::string::npos) return decimalLiteral(raw).value;
    // 含小数点的按定点数解析。
    const auto* start = raw.data();
    // 指向文本开头。
    const auto* end = start + raw.size();
    // 指向文本末尾之后。
    if (start != end && *start == '+') ++start;
    // 前缀正号对整数解析没有意义，跳过它。
    std::int64_t value{};
    const auto result = std::from_chars(start, end, value);
    // 文本转整数。
    if (result.ec != std::errc{} || result.ptr != end) invalid("unchecked integer literal");
    // 溢出或没解析完，说明这个字面量本应更早被拦住，属于内部不变量破坏。
    return value;
    // 返回整数值。
}

// 外层作用域快照。键仍是 "qualifier.name"——执行器 (database.cpp) 目前按这个
// 键把内层 AST 的标识符文本绑到外层行槽位上。值里额外带上 binding/relation，
// 执行器迁移到按身份绑定之后，字符串键即可退役。
nlohmann::json correlatedScope(const catalog::Table& table, const SlotMap& slots) {
    nlohmann::json scope = nlohmann::json::object();
    // 结果是一个对象。
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
    // 逐列处理。
        const auto& column = table.columns[index];
        // 当前列。
        if (column.qualifier.empty()) continue;
        // 没有限定名的列无法生成唯一键，跳过。
        const auto identity = slotAt(slots, index);
        scope[canonical(column.qualifier + "." + column.name)] =
            {{"columnId", index}, {"type", column.type},
             {"binding", identity.binding}, {"relation", identity.relation}};
    }
    return scope;
    // 返回映射。
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
// 判断一个表达式子树里是否含有子查询（EXISTS / IN / 标量）。
    if (expression.kind == "Exists" || expression.kind == "InSubquery" || expression.kind == "ScalarSubquery") return true;
    // 三个子查询节点直接命中。
    return (expression.left && containsSubquery(*expression.left)) || (expression.right && containsSubquery(*expression.right));
    // 否则递归检查左右子树。
}
bool containsNegatedSubquery(const Expr& expression) {
// 判断表达式里是否存在"被 NOT 包住的子查询"，这种形态去关联时要格外小心。
    if (expression.kind == "Unary" && canonical(expression.value) == "not" && expression.left && containsSubquery(*expression.left)) return true;
    // 当前节点是 NOT 且它的操作数里有子查询，即命中。
    return (expression.left && containsNegatedSubquery(*expression.left)) || (expression.right && containsNegatedSubquery(*expression.right));
    // 否则递归左右子树继续找。
}
nlohmann::json bindExpression(const Expr& expression, const catalog::Table& table,
                              const BindContext& context = {}, std::size_t depth = 0) {
    if (depth > 256) invalid("expression depth exceeded");
    // 深度保护，避免畸形表达式把递归栈打爆。
    nlohmann::json result = {{"kind", expression.kind}, {"line", expression.location.line},
                             // 先把节点种类与源位置搬过去，
                             {"column", expression.location.column}};
                             // 列号也一并记录，便于报错定位。
    if (context.bound) result["expressionId"] = rawId(context.bound->idFor(&expression));
    if (expression.kind == "Identifier") {
    // 列引用：要解析成列编号并带上列元数据。
        const auto index = columnIndex(table, expression.value);
        // 解析列下标（含歧义与不存在检测）。
        // columnId 是运行时槽位；binding/relation 是稳定身份。两者分开之后，
        // 裁剪与下推可以看身份，执行器继续看槽位。
        result["columnId"] = index;
        // 计划里用列编号取数，这是最关键的字段。
        const auto identity = slotAt(context.slots, index);
        result["binding"] = identity.binding;
        result["relation"] = identity.relation;
        result["name"] = table.columns[index].name;
        // 带上规范列名，便于展示与调试。
        result["type"] = table.columns[index].type;
        // 列类型，供上层做类型推导。
        result["nullable"] = table.columns[index].nullable;
        // 可空性，供三值逻辑与连接补 NULL 判断。
    } else if (expression.kind == "Literal") {
    // 字面量：把文本转成真正的 JSON 值并推断类型。
        result["value"] = literalValue(expression.value);
        // 文本转值。
        result["type"] = result["value"].is_null() ? "null" : result["value"].is_boolean() ? "bool" : result["value"].is_string() ? "varchar" : result["value"].is_number_float() ? "float" : "int";
        // 依次判断：null → 布尔 → 字符串 → 浮点 → 其余按整数。
        if (result["value"].is_number_integer() && (result["value"].get<std::int64_t>() < INT32_MIN || result["value"].get<std::int64_t>() > INT32_MAX)) result["type"] = "bigint";
        // 超出 32 位范围的整数要升级成 bigint，否则字面量装不下。
        result["nullable"] = result["value"].is_null();
        // 只有 NULL 字面量本身可空。
        const bool dateLiteral = dateLiteralText(expression.value).has_value();
        // 再单独判断它是不是日期字面量（文本形态上看）。
        const auto firstNumeric = [&]() {
        // 局部工具：判断去掉正负号后的第一个字符是不是数字。
            if (expression.value.empty()) return false;
            // 空串直接否。
            const std::size_t offset = expression.value.front() == '+' || expression.value.front() == '-' ? 1 : 0;
            // 有正负号就跳过第一位。
            return offset < expression.value.size() && std::isdigit(static_cast<unsigned char>(expression.value[offset]));
            // 判断该位是不是数字。
        };
        // firstNumeric 定义结束。
        if (dateLiteral) result["type"] = "date";
        // 日期字面量的类型是 date。
        if (!dateLiteral && firstNumeric() && expression.value.find_first_of("eE") != std::string::npos) result["type"] = "float";
        // 数字形态且含 e/E 的按浮点。
        if (firstNumeric() && expression.value.find_first_of("eE") == std::string::npos && expression.value.find('.') != std::string::npos) result["type"] = decimalLiteral(expression.value, expression.location).type.name();
        // 数字形态、不含指数、含小数点的按定点数，类型名用标度推导后的规范名。
    } else if (expression.kind == "Exists") {
    // EXISTS 子查询绑定。
        if (expression.subquerySql.empty()) invalid("missing EXISTS subquery");
        // EXISTS 必须带子查询原文，否则语法树不完整。
        result["subquerySql"] = expression.subquerySql;
        // 把子查询原文带进计划，执行期据此再解析。
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 写进计划供执行期做参数绑定。
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "SemiJoin" : "Apply";
        // 不引用外层列就是普通半连接；引用外层列必须走相关执行（Apply）。
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        // 相关情形标注去关联策略：按绑定参数分组物化，而不是逐行重算。
        result["type"] = "bool";
        // EXISTS 的结果类型是布尔。
        result["nullable"] = false;
        // 而且一定非空。
    } else if (expression.kind == "ScalarSubquery") {
    // 标量子查询。
        if (expression.subquerySql.empty()) invalid("missing scalar subquery");
        // 同样必须有子查询原文。
        result["subquerySql"] = expression.subquerySql;
        // 带进计划。
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 写进计划。
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "ScalarSubquery" : "Apply";
        // 不相关时是标量子查询算子，相关时走 Apply。
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        // 相关情形同样标注分组物化策略。
        result["type"] = "null";
        // 标量子查询的类型在编译期未知，先记为 null 表示"待定"。
        result["nullable"] = true;
        // 因为可能没有匹配行（结果为 NULL），所以允许为空。
    } else if (expression.kind == "InSubquery") {
    // IN 子查询。
        if (!expression.left || expression.subquerySql.empty()) invalid("missing IN subquery operand");
        // 必须同时有左操作数与子查询原文。
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        result["subquerySql"] = expression.subquerySql;
        // 带进计划。
        const auto outer = correlatedScope(table, context.slots);
        result["outerColumns"] = outer;
        // 写进计划。
        // 绑定不完整时不写该字段：下游据此区分「确实没有外层引用」与「没有绑定信息」。
        if (context.bound && context.bound->complete)
            result["outerReferences"] = outerReferences(context, expression);
        result["planKind"] = outer.empty() ? "SemiJoin" : "Apply";
        // 不相关时用半连接实现，相关时走 Apply。
        result["decorrelation"] = outer.empty() ? "none" : "grouped-parameter-instances";
        // 相关情形标注分组物化。
        result["type"] = "bool";
        // IN 的结果是布尔。
        result["nullable"] = true;
        // 左值为 NULL 或子查询为空时结果可能是 NULL，因此允许可空。
    } else if (expression.kind == "AggregateExpr") {
    // 聚合函数。
        if (!expression.left) invalid("missing aggregate argument");
        // 聚合必须带参数节点（COUNT(*) 的参数是通配节点）。
        result["function"] = expression.value;
        // 记下函数名（已是大写）。
        result["left"] = expression.left->kind == "Wildcard" ? nlohmann::json(nullptr) : bindExpression(*expression.left, table, context, depth + 1);
        result["type"] = expression.value == "COUNT" || expression.value == "SUM" ? "bigint" :
            // COUNT 与 SUM 统一按 bigint 处理，
            expression.value == "AVG" ? "decimal(38,6)" : result["left"].at("type").get<std::string>();
            // AVG 固定给一个足够宽的定点数类型；MIN/MAX 则与原参数同类型。
        result["nullable"] = expression.value != "COUNT";
        // COUNT 一定返回数字；其余聚合在空集时可能返回 NULL。
        if (expression.value == "SUM" || expression.value == "AVG") {
        // SUM 与 AVG 还要看参数类型做细调。
            if (result["left"].at("type") == "float") result["type"] = "float";
            // 浮点求和仍是浮点。
            else if (const auto decimal = decimalType(result["left"].at("type").get<std::string>()))
            // 定点数参数要重新算标度。
                result["type"] = DecimalType{38, expression.value == "SUM" ? decimal->scale : std::max(6u, decimal->scale)}.name();
                // 宽度统一放大到 38 位；SUM 保留原标度，AVG 至少保留 6 位小数。
        }
        // WHERE 表达式收集循环结束。
        // 类型细化结束。
    } else if (expression.kind == "Cast") {
    // 类型转换。
        if (!expression.left) invalid("missing CAST operand");
        // 必须有被转换的表达式。
        result["type"] = canonical(expression.value);
        // 目标类型取规范名（小写）。
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        result["nullable"] = result["left"].value("nullable", true);
        // 转换不会改变可空性，沿用源表达式的。
    } else if (expression.kind == "Unary" || expression.kind == "Binary") {
    // 一元或二元运算。
        result["operator"] = expression.value;
        // 记下运算符。
        result["type"] = isArithmetic(expression.value) ? "int" : "bool";
        // 先粗判类型：算术运算给 int，其余（比较、逻辑）给 bool；下面再细化。
        if (!expression.left) invalid("missing left operand");
        // 必须有左操作数。
        result["left"] = bindExpression(*expression.left, table, context, depth + 1);
        if (expression.kind == "Binary") {
        // 二元运算还有右操作数。
            if (!expression.right) invalid("missing right operand");
            // 必须存在。
            result["right"] = bindExpression(*expression.right, table, context, depth + 1);
        }
        // 右操作数处理结束。
        result["nullable"] = expression.value != "IS NULL" && expression.value != "IS NOT NULL" &&
            // IS NULL / IS NOT NULL 的结果一定非空，其余运算要看操作数。
            (result["left"].value("nullable", true) || (expression.kind == "Binary" && result["right"].value("nullable", true)));
            // 只要任一操作数可空，结果就可空。
        if (isArithmetic(expression.value) && (result["left"].at("type") == "bigint" || (expression.kind == "Binary" && result["right"].at("type") == "bigint"))) result["type"] = "bigint";
        // 算术运算只要有一侧是 bigint，结果就按 bigint 算，避免溢出。
        if (isArithmetic(expression.value)) {
        // 算术运算还要按操作数类型进一步细化结果类型。
            if (result["left"].at("type") == "float" || (expression.kind == "Binary" && result["right"].at("type") == "float")) {
            // 任一侧是浮点。
                result["type"] = "float";
                // 结果就是浮点，不再往下判断。
            } else {
            // 没有浮点参与，考虑定点数。
            const auto a = decimalType(result["left"].at("type").get<std::string>());
            // 左侧是不是定点数，是就取出精度与标度。
            const auto b = expression.kind == "Binary" ? decimalType(result["right"].at("type").get<std::string>()) : std::nullopt;
            // 二元运算时再看右侧；一元运算没有右操作数。
            if (expression.kind == "Unary" && a) result["type"] = a->name();
            // 一元取负不改变定点数类型。
            else if (a || b) result["type"] = decimalArithmeticType(expression.value, a ? a->scale : 0, b ? b->scale : 0, expression.location).name();
            // 定点数参与运算时，按运算符与两侧标度推导结果类型（加减取较大标度、乘加标度等）。
            }
            // 分支结束。
        }
        // 算术类型细化结束。
    } else {
    // 其它节点种类目前没有实现。
        invalid("unsupported expression " + expression.kind);
        // 按内部不变量破坏处理：说明语法树里出现了计划层不认识的节点。
    }
    // 所有分支结束。
    return result;
    // 返回绑定好的计划表达式。
}

std::vector<PlanColumn> schema(const catalog::Table& table, const SlotMap& slots = {}) {
    std::vector<PlanColumn> output;
    // 结果列表。
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
    // 逐列转换，顺序必须与目录一致（列编号就是它的位置）。
        const auto identity = slotAt(slots, i);
        output.push_back({table.columns[i].name, table.columns[i].type, i, table.columns[i].nullable, table.columns[i].defaultValue,
            // 依次填入列名、类型、列编号、可空性与默认值，
            table.columns[i].primaryKey, table.columns[i].unique, table.columns[i].references,
            identity.binding, identity.relation});
    }
    return output;
    // 返回列描述列表。
}

nlohmann::json expressionIdentity(nlohmann::json value) {
// 计算表达式的"结构身份"：抹掉源位置后剩下的内容，用于判断两个表达式是否等价。
    if (!value.is_object()) return value;
    // 非对象（例如 null 常量）直接返回。
    value.erase("line"); value.erase("column");
    // 去掉行号列号，这样同一表达式在不同位置也视为相同。
    // X25 回归修复：binding/relation/expressionId 是 X25 稳定绑定身份的"实例级"编号，
    // 同一列引用在不同位置（GROUP BY 键 vs 投影）必然不同；结构身份必须抹掉，
    // 否则 lowerAggregate 的分组键匹配永远失配，裸列全部误报 2003。
    value.erase("binding"); value.erase("relation"); value.erase("expressionId");
    if (value.contains("left")) value["left"] = expressionIdentity(value["left"]);
    // 递归规范化左子树。
    if (value.contains("right")) value["right"] = expressionIdentity(value["right"]);
    // 递归规范化右子树。
    return value;
    // 返回规范化后的表达式。
}
void lowerAggregate(LogicalPlan& project, const Statement& statement, const catalog::Table& scope,
                    const BindContext& context) {
    if (project.kind != "Project" || project.children.size() != 1) invalid("aggregate requires a projection input");
    // 只处理"投影且恰好一个输入"这种规整形态。
    LogicalPlan aggregate;
    // 准备要插入的聚合算子。
    aggregate.kind = "Aggregate";aggregate.table = project.table;
    // 种类是聚合，表名沿用投影的。
    std::unordered_map<std::string, std::size_t> groups, functions;
    // 两张映射：分组键身份 → 槽位；聚合调用身份 → 槽位。
    for (const auto& key : statement.groupBy) {
    // 逐个处理 GROUP BY 分组键。
        auto expression = bindExpression(*key, scope, context);
        const auto identity = expressionIdentity(expression).dump();
        // 计算它的结构身份，用于去重。
        if (groups.contains(identity)) continue;
        // 同一个分组键写两遍只保留一份。
        const auto index = aggregate.output.size();
        // 这个分组键在聚合输出里的槽位号。
        groups.emplace(identity, index);
        // 登记映射。
        aggregate.output.push_back({"_group_" + std::to_string(index), expression.at("type").get<std::string>(), index, expression.value("nullable", true)});
        // 分配一个输出列，名字形如 _group_0，便于在计划里辨认。
        aggregate.groupKeys.push_back(std::move(expression));
        // 把绑定好的分组键放进聚合算子。
    }
    // 分组键处理结束。
    const auto reference = [&](std::size_t index, const nlohmann::json& expression) {
    // 局部工具：构造"引用聚合输出第 index 列"的列引用表达式。
        const auto& column = aggregate.output.at(index);
        // 取出该槽位的列描述。
        return nlohmann::json{{"kind", "Identifier"}, {"columnId", index}, {"name", column.name},
            // 种类是列引用，列编号就是槽位号，名字用槽位名。
            {"type", column.type}, {"nullable", column.nullable},
            // 类型与可空性沿用槽位描述。
            {"line", expression.value("line", std::size_t{0})}, {"column", expression.value("column", std::size_t{0})}};
            // 位置信息从原表达式继承，保证报错仍指向用户写的那一处。
    };
    // reference 定义结束。
    // 聚合以上的表达式只引用分组键或聚合槽位，不保留原始行列引用。
    // 聚合以上的表达式只引用分组键或聚合槽位，不保留原始行列引用。
    std::function<nlohmann::json(nlohmann::json, std::size_t)> rewrite;
    // 先声明再赋值，是为了让 lambda 能递归调用自己。
    rewrite = [&](nlohmann::json expression, std::size_t depth) -> nlohmann::json {
    // 递归改写表达式。
        if (depth > 256) invalid("aggregate expression depth exceeded");
        // 深度保护。
        const auto identity = expressionIdentity(expression).dump();
        // 计算当前表达式的结构身份。
        if (const auto group = groups.find(identity); group != groups.end()) return reference(group->second, expression);
        // 如果它整体就是某个分组键，直接换成对该槽位的引用。
        const auto kind = expression.at("kind").get<std::string>();
        // 否则看节点种类。
        if (kind == "AggregateExpr") {
        // 聚合调用。
            auto found = functions.find(identity);
            // 先查这个聚合调用是否已经登记过。
            if (found == functions.end()) {
            // 没登记过才新建槽位。
                const auto index = aggregate.output.size();
                // 新槽位号。
                found = functions.emplace(identity, index).first;
                // 登记映射并拿到迭代器。
                aggregate.output.push_back({"_aggregate_" + std::to_string(index), expression.at("type").get<std::string>(), index, expression.value("nullable", true)});
                // 分配输出列，名字形如 _aggregate_0。
                aggregate.aggregates.push_back({{"function", expression.at("function")}, {"argument", expression.at("left")},
                    // 把聚合函数与它的参数登记进算子，
                    {"columnId", index}, {"type", expression.at("type")}, {"nullable", expression.at("nullable")}});
                    // 以及输出槽位、结果类型与可空性。
            }
            // 登记结束。
            return reference(found->second, expression);
            // 返回对该聚合槽位的引用。
        }
        // 聚合分支结束。
        if (kind == "Identifier") throw MiniSqlError(ErrorCode::Semantic, "Column must be grouped or aggregated: " + expression.at("name").get<std::string>(),
            // 尚未被改写的裸列引用，说明它既不在 GROUP BY 也不在聚合函数里，按 SQL 语义报错。
            {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
            // 错误位置取自该列引用自身。
        if (expression.contains("left")) expression["left"] = rewrite(expression.at("left"), depth + 1);
        // 递归改写左子表达式。
        if (expression.contains("right")) expression["right"] = rewrite(expression.at("right"), depth + 1);
        // 递归改写右子表达式。
        return expression;
        // 返回改写后的表达式。
    };
    // rewrite 定义结束。
    if (project.projections.empty()) for (const auto& column : project.output)
    // 投影列表为空时（例如 SELECT * 展开前的形态），按输出列逐个补出列引用投影。
        project.projections.push_back(bindExpression(Expr{"Identifier", column.name, {}, {}, statement.location}, scope, context));
    for (std::size_t i = 0; i < project.projections.size(); ++i) {
    // 逐条处理投影表达式。
        auto& expression = project.projections[i];
        // 取引用。
        expression = rewrite(expression, 0);
        // 用上面的 rewrite 把分组键与聚合调用替换成对聚合槽位的引用。
        project.output.at(i).columnId = expression.at("kind") == "Identifier" ? expression.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
        // 投影结果若正好是一个列引用，就把它的列编号记进输出元数据；
        // 否则填 -1 表示"不是对某个输入列的直通"。
    }
    // 投影处理结束。
    auto having = statement.having ? rewrite(bindExpression(*statement.having, scope, context), 0) : nlohmann::json(nullptr);
    // X09 4.x: 聚合之上（HAVING / 投影）的相关子查询 —— 其 outerColumns 携带的是基表列下标，
    // 而此处实际求值的行是聚合输出行（分组键 + 聚合槽位）。必须把外层列下标重映射到 GROUP BY
    // 键在 aggregate.output 中的槽位，否则执行期会越界（5001）或取自错误列。
    // 引用未参与分组的列按 SQL 语义报 2003，与 rewrite 对普通标识符的处理一致。
    std::unordered_map<std::size_t, std::size_t> groupSlot;
    // 基表列编号 → 分组键在聚合输出中的槽位。
    for (std::size_t i = 0; i < aggregate.groupKeys.size(); ++i) {
    // 逐个分组键建立映射。
        const auto& key = aggregate.groupKeys[i];
        // 当前分组键表达式。
        if (key.is_object() && key.value("kind", "") == "Identifier" && key.contains("columnId"))
        // 只有"分组键就是一个列引用"的情况才有确定的基表列编号。
            groupSlot.emplace(key.at("columnId").get<std::size_t>(), i);
            // 记下"该基表列 → 第 i 个聚合输出槽位"。
    }
    // 映射建立结束。
    std::function<void(nlohmann::json&)> remapGroupRefs;
    // 递归重映射函数：就地修改表达式里的 outerColumns。
    remapGroupRefs = [&](nlohmann::json& node) {
    // 处理一个节点。
        if (!node.is_object()) return;
        // 非对象直接返回。
        const auto kind = node.value("kind", "");
        // 节点种类。
        if ((kind == "ScalarSubquery" || kind == "Exists" || kind == "InSubquery") &&
            // 只处理三种子查询节点，
            node.contains("subquerySql") && node.at("subquerySql").is_string() &&
            // 且必须带子查询原文，
            node.contains("outerColumns") && node.at("outerColumns").is_object()) {
            // 以及外层列映射。
            std::map<std::string, std::size_t> slots;
            // 外层列名 → 新的聚合输出槽位。
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
                // 报错分支结束。
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
            // 逐个处理结束。
            for (const auto& entry : slots) node.at("outerColumns").at(entry.first)["columnId"] = entry.second;
            // 把新槽位号写回 outerColumns，执行期就按它取聚合输出行。
        }
        // 子查询节点处理结束。
        if (node.contains("left")) remapGroupRefs(node["left"]);
        // 递归处理左子树。
        if (node.contains("right")) remapGroupRefs(node["right"]);
        // 递归处理右子树。
    };
    // remapGroupRefs 定义结束。
    for (auto& expression : project.projections) remapGroupRefs(expression);
    // 对每条投影表达式做重映射。
    if (!having.is_null()) remapGroupRefs(having);
    // HAVING 也要重映射。
    aggregate.children.push_back(std::move(project.children.front()));
    // 把投影原来的输入（扫描/过滤/连接）挂到聚合算子下面，作为它的数据来源。
    project.children.clear();
    // 清空投影的孩子，准备换成"聚合算子（上面还可能有一层 HAVING 过滤）"。
    if (statement.having) {
    // 有 HAVING 时要在聚合之上再插一层过滤。
        LogicalPlan filter;
        // 新建过滤算子。
        filter.kind = "Filter";filter.table = aggregate.table;filter.output = aggregate.output;
        // 种类是过滤；表名与输出列都沿用聚合算子（过滤不改变结果结构）。
        filter.predicate = std::move(having);filter.children.push_back(std::move(aggregate));
        // 谓词就是 HAVING 表达式，输入是聚合算子。
        project.children.push_back(std::move(filter));
        // 投影之下挂这层过滤。
    } else project.children.push_back(std::move(aggregate));
    // 没有 HAVING 时投影直接架在聚合上。
}
LogicalPlan build(const Statement& statement, const catalog::Catalog& catalog, const BindResult& bound) {
    if (statement.kind == "Begin" || statement.kind == "Commit" || statement.kind == "Rollback" ||
        // 事务控制类语句
        statement.kind == "Savepoint" || statement.kind == "ReleaseSavepoint" || statement.kind == "RollbackTo") {
        // 统一处理，计划节点只是原样承载种类与保存点名。
        LogicalPlan plan;plan.kind = statement.kind;plan.savepointName = statement.savepointName;return plan;
        // 构造一个"动作型"计划节点直接返回，没有输入也没有输出列。
    }
    // 事务语句处理结束。
    if (statement.kind == "Checkpoint") { LogicalPlan plan; plan.kind = "Checkpoint"; return plan; }
    // 检查点语句：同样只做一个动作节点。
    if (statement.kind == "DropIndex") { LogicalPlan plan; plan.kind = "DropIndex"; plan.table = statement.table; plan.indexName = statement.indexName; return plan; }
    // 删索引：带上表名与索引名。
    if (statement.kind == "CreateIndex") { LogicalPlan plan; plan.kind = "CreateIndex"; plan.table = statement.table; plan.indexName = statement.indexName; plan.uniqueIndex = statement.uniqueIndex; plan.indexColumns = statement.indexColumns; return plan; }
    // 建索引：带上表名、索引名、是否唯一以及索引列。
    // X09 3.3: 派生表作为外层关系基座。内层 select 计划先构建，其实体输出
    // X09 3.3：FROM 派生表作为外层查询的关系基座。
    // （selectItems）成为外层查询的绑定作用域；未限定列名一律按该作用域解析，禁字符串替换。
    // 内层 select 的计划要先构建出来，它的实体输出（selectItems）成为外层查询的绑定作用域；
    // 未限定列名一律按该作用域解析，禁止用字符串替换的土办法伪装实现。
    // X09 3.3: 派生表作为外层关系基座。内层 select 计划先构建，其实体输出
    // （selectItems）成为外层查询的绑定作用域；未限定列名一律按该作用域解析，禁字符串替换。
    const bool derivedBase = statement.fromSubquery != nullptr;
    // 这条语句的 FROM 是不是派生表。
    const auto* table = derivedBase ? nullptr : catalog.find(statement.table);
    // 不是派生表时去目录里找真实表定义。
    if (!table && !derivedBase) invalid("missing table " + statement.table);
    // 既不是派生表又找不到表定义，说明编译流程有漏，按内部不变量破坏处理。
    LogicalPlan plan;
    // 正在构建的计划。
    catalog::Table bindScope;
    // 用于绑定列名的作用域表。
    LogicalPlan derivedInput;
    // 派生表对应的内层计划。
    if (derivedBase) {
    // 分支一：FROM 是派生表。
        derivedInput = build(*statement.fromSubquery, catalog, bound);
        plan.table = statement.tableAlias.empty() ? statement.table : statement.tableAlias;
        // 外层看到的名字用别名（没写别名时退化成占位表名）。
        catalog::Table derived;
        // 把内层计划的输出列包装成一张"虚拟表"，作为外层的作用域。
        derived.name = plan.table;
        // 虚拟表名就是别名。
        for (const auto& column : derivedInput.output)
        // 逐列转换。
            derived.columns.push_back({column.name, column.type, derived.name, column.nullable, column.defaultValue, column.primaryKey, column.unique, column.references});
            // 列名、类型、限定名（别名）、可空性、默认值、主键/唯一标记与外键都从内层输出继承。
        bindScope = std::move(derived);
        // 设为绑定作用域。
        for (const auto& join : statement.joins) {
            const auto* right = catalog.find(join.table);
            if (!right) invalid("missing join table");
            const auto qualifier = join.alias.empty() ? right->name : join.alias;
            if (join.right) for (auto& column : bindScope.columns) column.nullable = true;
            for (auto column : right->columns) { column.qualifier = qualifier; if (join.left) column.nullable = true; bindScope.columns.push_back(std::move(column)); }
        }
    } else {
    // 分支二：FROM 是普通表。
        plan.table = table->name;
        // 用目录里的规范表名。
        for (const auto& check : table->checks)
        // 逐条 CHECK 约束。
            plan.checks.push_back(bindExpression(*deserializeExpression(nlohmann::json::parse(check)), *table));
            // 磁盘上存的是表达式文本，这里解析回语法树再绑定成计划表达式。
        bindScope = catalog::queryScope(statement, catalog);
        // 把主表与本次用到的 JOIN 合并成视图表，作为列解析的统一起点。
    }
    // 作用域准备结束。
    // 该语句在绑定结果里的作用域；绑定失败或语句不在结果中时 slots 全 0，
    // 计划仍按槽位语义构建，只是没有稳定身份。
    const auto* boundStatement = bound.statementFor(&statement);
    const auto slots = slotIdentities(bindScope, bound, boundStatement ? boundStatement->scope : ScopeId::Invalid);
    const BindContext context{&bound, slots};
    const bool aggregated = statement.kind == "Select" && catalog::analyzeSelect(statement, bindScope).aggregated;
    // 先问一次语义层：这条 SELECT 是不是聚合查询（有 GROUP BY/HAVING/聚合函数）。
    // 后面决定是否插入聚合算子、以及扫描能否走索引都依赖这个结论。
    if (statement.kind == "CreateTable") {
    // 建表。
        plan.kind = "CreateTable";
        // 计划种类。
        plan.checkDefinitions = serializeChecks(statement.checks);
        // CHECK 的原始定义（约束名与目标绑定）要落库，这里序列化保存。
        plan.keys = table->keys;
        // 主键/唯一键约束取自校验后的目录。
        plan.foreignKeys = table->foreignKeys;
        // 外键同理。
        plan.constraintNames = table->constraintNames;
        // 约束命名绑定也一并落库。
        plan.output = schema(*table);
        // 输出列描述用于告知上层这张表长什么样。
    } else if (statement.kind == "Insert") {
    // 插入。
        plan.kind = "Insert";
        // 计划种类。
        if (!statement.valueRows.empty()) {
        // 多行插入：拆成若干单行分别编译，再合成一个计划。
            auto single = statement;
            // 复制语句作为单行模板。
            single.valueRows.clear();
            // 去掉多行结构。
            single.values.clear();
            // 清掉旧的单行值。
            for (const auto& row : statement.valueRows) {
            // 逐行处理。
                single.valueExpressions = row;
                // 把这一行装进模板。
                const auto item = build(single, catalog, bound);
                plan.insertRows.push_back({{"values", item.values}, {"expressions", item.insertExpressions}});
                // 把该行的字面量值与表达式两种表示都收进多行计划。
                plan.columnMapping = item.columnMapping;
                // 列映射各行一致，取任意一行的即可。
            }
            // 行处理结束。
            return plan;
            // 多行插入计划构建完毕，直接返回。
        }
        // 多行分支结束。
        plan.values = nlohmann::json::array();
        // 单行插入：先准备一个"按建表顺序排列的默认值数组"。
        if (!statement.valueExpressions.empty())
        // 表达式形式的值需要准备对应的表达式槽位。
            for (std::size_t i = 0; i < table->columns.size(); ++i)
            // 每一列先填上它自己的默认值。
                plan.insertExpressions.push_back(bindExpression(Expr{"Literal", table->columns[i].defaultValue.value_or("NULL"), {}, {}, statement.location}, catalog::Table{"", {}}));
                // 没有默认值就填 NULL；空表作用域表示这里只允许常量表达式。
        for (const auto& column : table->columns) plan.values.push_back(literalValue(column.defaultValue.value_or("NULL")));
        // 字面量形式的值同样先按默认值铺满。
        const auto names = catalog::insertColumns(statement, *table);
        // 算出本次真正要写入哪些列（用户写了列清单就用它，否则按全表顺序）。
        for (std::size_t i = 0; i < names.size(); ++i) {
        // 把用户给的第 i 个值装到对应列上。
            const auto index = columnIndex(*table, names[i]);
            // 列名转列下标。
            plan.columnMapping.push_back(index);
            // 记录"第 i 个值写进第 index 列"。
            if (statement.valueExpressions.empty()) plan.values[index] = literalValue(statement.values[i]);
            // 字面量形式：把文本转成值放进去。
            else {
            // 表达式形式：要单独处理每一列的表达式。
                const auto& expression = *statement.valueExpressions[i];
                // 取该列的表达式节点。
                if (expression.kind == "Default") {
                // 写的是 DEFAULT 关键字。
                    const auto raw = table->columns[index].defaultValue.value_or("NULL");
                    // 取出该列的默认值文本。
                    plan.insertExpressions[index] = bindExpression(Expr{"Literal", raw, {}, {}, expression.location}, catalog::Table{"", {}});
                    // 用默认值替换成一个字面量表达式。
                    plan.values[index] = literalValue(raw);
                    // 字面量形式的值也一并填好。
                } else plan.insertExpressions[index] = bindExpression(expression, catalog::Table{"", {}});
                // 其它情况直接绑定表达式；空表作用域保证它不会引用列。
                if (expression.kind == "Literal") plan.values[index] = literalValue(expression.value);
                // 如果本来就是字面量，再把它的值也抄进 values，供按字面量执行的路径使用。
            }
            // 该列处理结束。
        }
        // 所有列处理结束。
    } else if (statement.kind == "Select" || statement.kind == "Delete" || statement.kind == "Update") {
    // 分支三：查询三类数据语句，它们共用"输入关系 + 过滤 + 投影/修改"的骨架。
        LogicalPlan input;
        // 这三类语句共同的输入算子。
        if (derivedBase) {
        // 派生表基座。
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
        // 普通表：从一次顺序扫描开始。
        LogicalPlan scan;
        // 扫描算子。
        scan.kind = "SeqScan";
        // 默认是顺序扫描。
        scan.table = table->name;
        // 扫哪张表。
        scan.output = schema(*table, slotWindow(slots, 0, table->columns.size()));
        scan.preservesRowId = true;
        // 扫描保留物理行号，这样上层仍能定位到具体行（删除/更新要靠它回写）。
        if (statement.kind == "Select" && !aggregated && statement.joins.empty() && statement.where) {
        // 只有在"单表 SELECT、非聚合、有 WHERE"时才尝试把过滤转成索引扫描。
            std::unordered_map<std::string, std::shared_ptr<Expr>> equalityLiterals;
            // 收集形如 列 = 常量 的条件。
            std::unordered_map<std::string, std::pair<std::string, std::shared_ptr<Expr>>> rangeLiterals;
            // 收集形如 列 < 常量 的范围条件（记录运算符与常量）。
            std::function<void(const Expr&)> collect = [&](const Expr& expression) {
            // 递归拆解 WHERE 的 AND 链，把可用条件抽取出来。
                if (expression.kind == "Binary" && expression.value == "AND" && expression.left && expression.right) {
                // 遇到 AND 就两边分别递归。
                    collect(*expression.left);
                    // 左半分。
                    collect(*expression.right);
                    // 右半分。
                    return;
                    // 处理完返回。
                }
                // AND 分支结束。
                if (expression.kind != "Binary" || !expression.left || !expression.right ||
                // 只关心二元比较，
                    expression.left->kind != "Identifier" || expression.right->kind != "Literal") return;
                    // 而且必须是"列 与 字面量"这种最简形式，否则无法直接做索引查找。
                const auto name = canonical(expression.left->value);
                // 取列名的小写形式作为键。
                if (expression.value == "=") equalityLiterals[name] = expression.right;
                // 等值条件记进等值表。
                else if (expression.value == "<" || expression.value == "<=" || expression.value == ">" || expression.value == ">=")
                // 四种比较运算符属于范围条件。
                    rangeLiterals[name] = {expression.value, expression.right};
                    // 记下运算符与边界常量。
            };
            // collect 定义结束。
            collect(*statement.where);
            // 从整条 WHERE 开始收集。
            for (const auto& index : table->indexes) {
            // 逐条索引，看能不能用上。
                if (index.columns.empty()) continue;
                // 空索引跳过。
                std::vector<std::shared_ptr<Expr>> values;
                // 命中的等值前缀。
                std::size_t prefix = 0;
                // 前缀长度。
                for (; prefix < index.columns.size(); ++prefix) {
                // 贪心匹配索引列的等值条件。
                    const auto found = equalityLiterals.find(canonical(index.columns[prefix]));
                    // 查这一列有没有等值条件。
                    if (found == equalityLiterals.end()) break;
                    // 没有就停下，前缀到此为止。
                    values.push_back(found->second);
                    // 有就把常量收进前缀值列表。
                }
                // 等值前缀匹配结束。
                bool selected = prefix == index.columns.size();
                // 覆盖了索引的全部列，属于完整等值查找。
                std::pair<std::string, std::shared_ptr<Expr>> range;
                // 待用的一步范围条件。
                if (!selected && prefix < index.columns.size()) {
                // 等值没覆盖全，再看下一列能不能用范围条件。
                    const auto found = rangeLiterals.find(canonical(index.columns[prefix]));
                    // 查该列的范围条件。
                    if (found != rangeLiterals.end()) { range = found->second; selected = true; }
                    // 找到就认为这条索引可用（等值前缀 + 一步范围）。
                }
                // 范围判断结束。
                if (!selected) continue;
                // 这条索引用不上，换下一条。
                scan.kind = "IndexScan";
                // 把顺序扫描改写成索引扫描。
                scan.indexName = index.name;
                // 使用的索引名。
                scan.indexColumns = index.columns;
                // 索引列。
                scan.indexValues = nlohmann::json::array();
                // 等值查找的值列表。
                for (const auto& value : values) scan.indexValues.push_back(bindExpression(*value, bindScope, context));
                if (prefix < index.columns.size()) {
                // 如果还用了范围条件。
                    scan.indexRangeOperator = range.first;
                    // 记下运算符。
                    scan.indexRangeValue = bindExpression(*range.second, bindScope, context);
                }
                // 范围条件处理结束。
                break;
                // 一条能用的索引就够了，跳出循环。
            }
            // 索引尝试结束。
        }
        // 索引改写判断结束。
        input = std::move(scan);
        // 把扫描（或索引扫描）设为输入。
        }
        // 普通表分支结束。
        for (std::size_t i = 0; i < statement.joins.size(); ++i) {
        // 按书写顺序逐个叠加连接，每次把"当前输入"与"右边新表"接起来。
            const auto& source = statement.joins[i];
            // 当前连接子句。
            const auto* right = catalog.find(source.table);
            // 取右表定义。
            if (!right) invalid("missing join table");
            // 目录里找不到，说明编译流程有漏，按内部错误处理。
            catalog::Table prefix;
            const auto rightOffset = input.output.size();
            prefix.name = bindScope.name;
            prefix.columns.assign(bindScope.columns.begin(), bindScope.columns.begin() + static_cast<std::ptrdiff_t>(rightOffset + right->columns.size()));
            LogicalPlan rightScan;
            // 右侧的扫描算子。
            rightScan.kind = "SeqScan";rightScan.table = right->name;
            // 顺序扫描右表。
            rightScan.output = schema(*right, slotWindow(slots, rightOffset, right->columns.size()));
            rightScan.preservesRowId = true;
            LogicalPlan join;
            // 连接算子。
            join.kind = source.left && source.right ? "FullJoin" : source.left ? "LeftJoin" : source.right ? "RightJoin" : "NestedLoopJoin";join.table = plan.table;
            join.output = input.output;
            if (source.right) for (auto& column : join.output) column.nullable = true;
            auto rightOutput = rightScan.output;
            if (source.left) for (auto& column : rightOutput) column.nullable = true;
            join.output.insert(join.output.end(), rightOutput.begin(), rightOutput.end());
            join.predicate = bindExpression(*source.on, prefix, context);
            join.children.push_back(std::move(input));join.children.push_back(std::move(rightScan));
            // 左边挂当前输入，右边挂右表扫描。
            input = std::move(join);
            // 连接结果成为新的"当前输入"，供下一个连接继续叠加。
        }
        if (statement.where) {
        // 有 WHERE 时在输入之上加一层过滤。
            LogicalPlan filter;
            // 新建过滤算子。
            filter.kind = containsNegatedSubquery(*statement.where) ? "AntiJoin" :
                // 谓词里有"被 NOT 包住的子查询"时用反连接（AntiJoin）表达，
                containsSubquery(*statement.where) ? "SemiJoin" : "Filter";
                // 普通子查询用半连接（SemiJoin）；完全不含子查询就用普通过滤。
            filter.subqueryJoinKind = containsNegatedSubquery(*statement.where) ? "AntiJoin" :
                // 同时把种类名记进 subqueryJoinKind：
                containsSubquery(*statement.where) ? "SemiJoin" : "";
                // 这样后续优化器可以据此把节点提升成带类型的子查询节点。
            filter.table = plan.table;
            // 表名沿用外层。
            filter.output = input.output;
            // 过滤不改变结果结构，输出列沿用输入。
            filter.preservesRowId = input.preservesRowId;
            // 行号保持性也沿用输入。
            filter.predicate = bindExpression(*statement.where, bindScope, context);
            filter.children.push_back(std::move(input));
            // 输入挂在下面。
            input = std::move(filter);
            // 过滤结果成为新的输入。
        }
        // WHERE 处理结束。
        plan.kind = statement.kind == "Select" ? "Project" : statement.kind;
        // SELECT 是一个投影算子；DELETE/UPDATE 的节点种类与语句一致。
        if (statement.kind == "Update") {
        // UPDATE 要把每个赋值项的右值算出来，写进投影列表。
            for (const auto& item : statement.assignments) {
            // 逐条赋值处理。
                const auto index = columnIndex(*table, item.column);
                // 被赋值列的列下标。
                plan.columnMapping.push_back(index);
                // 记录"第几个投影对应哪一列"。
                if (item.expression->kind == "Default")
                // 写的是 DEFAULT 关键字。
                    plan.projections.push_back(bindExpression(Expr{"Literal", table->columns[index].defaultValue.value_or("NULL"), {}, {}, item.expression->location}, bindScope, context));
                else plan.projections.push_back(bindExpression(*item.expression, bindScope, context));
            }
            // 赋值处理结束。
        }
        // UPDATE 分支结束。
        if (statement.kind == "Select") {
        // SELECT 要构造输出列与投影表达式。
            if (!statement.selectItems.empty()) {
            // 有结构化投影列表时以它为准。
                for (std::size_t i = 0; i < statement.selectItems.size(); ++i) {
                // 逐项处理。
                    const auto& item = statement.selectItems[i];
                    // 当前投影项。
                    if (item.expression->kind == "Wildcard") {
                    // 通配项需要展开成若干真实列。
                        const auto dot = item.expression->value.find('.');
                        // 看它是不是"表名.*"形式。
                        for (const auto& column : schema(bindScope, slots)) {
                            const auto& source = bindScope.columns[column.columnId];
                            // 取出对应源列（用于看它的限定名）。
                            if (dot != std::string::npos && canonical(source.qualifier) != canonical(item.expression->value.substr(0, dot))) continue;
                            // 写了表名前缀时，只保留属于该表的列。
                            plan.output.push_back(column);
                            // 该列成为输出列之一。
                            Expr reference{"Identifier", source.qualifier + "." + column.name, {}, {}, item.expression->location};
                            // 造一个带限定名的列引用表达式。
                            plan.projections.push_back(bindExpression(reference, bindScope, context));
                        }
                        // 展开结束。
                        continue;
                        // 处理下一项。
                    }
                    // 通配分支结束。
                    auto bound = bindExpression(*item.expression, bindScope, context);
                    auto name = item.alias;
                    // 输出列名优先用别名。
                    if (name.empty()) name = item.expression->kind == "Identifier" ? bound.at("name").get<std::string>() : "expr_" + std::to_string(i + 1);
                    // 没写别名时：列引用用列名，其它表达式生成 expr_N 这样的占位名。
                    const auto columnId = item.expression->kind == "Identifier" ? bound.at("columnId").get<std::size_t>() : static_cast<std::size_t>(-1);
                    // 直通列引用才记下真实列编号，否则填 -1。
                    PlanColumn projected{name, bound.at("type").get<std::string>(), columnId, bound.value("nullable", true)};
                    // 投影列继承被投影标识符的稳定身份；表达式列没有身份（0）。
                    projected.binding = bound.value("binding", std::uint32_t{0});
                    projected.relation = bound.value("relation", std::uint32_t{0});
                    projected.expression = bound.value("expressionId", std::uint32_t{0});
                    plan.output.push_back(std::move(projected));
                    plan.projections.push_back(std::move(bound));
                    // 绑定结果收进投影列表。
                }
                // 投影项处理结束。
            } else if (!derivedBase) for (const auto& name : statement.selectList) {
            // 没有结构化投影（老路径）且不是派生表时，用字符串形式的投影列表。
                if (name == "*") plan.output = schema(*table, slots);
                else {
                // 具体列名。
                    auto index = columnIndex(*table, name);
                    // 解析列下标。
                    const auto identity = slotAt(slots, index);
                    PlanColumn column{table->columns[index].name, table->columns[index].type, index};
                    column.binding = identity.binding;
                    column.relation = identity.relation;
                    plan.output.push_back(std::move(column));
                }
                // 分支结束。
            }
            // 投影构造结束。
        }
        // SELECT 分支结束。
        plan.children.push_back(std::move(input));
        // 把前面搭好的输入（扫描/连接/过滤）挂到本算子下面。
    } else {
    // 走到这里说明遇到了计划层不认识的语句种类。
        invalid("unsupported statement " + statement.kind);
        // 按内部不变量破坏处理。
    }
    // 语句主体分派结束。
    nlohmann::json sortKeys = nlohmann::json::array();
    // 排序键列表（按投影列表的下标引用）。
    auto visibleOutput = plan.output;
    // 记下"用户可见的输出列"，后面插入聚合算子时要用它把可见列还原回去。
    if (statement.kind == "Select" && !statement.orderBy.empty()) {
    // 处理 ORDER BY。
        if (plan.projections.empty() && !derivedBase) {
        // 没有投影表达式（例如 SELECT *）时，先为每个输出列补一条列引用投影，
        // 这样排序键才能按投影下标定位。
            for (const auto& column : plan.output) {
            // 逐个输出列。
                Expr reference{"Identifier", column.name, {}, {}};
                // 造一个裸列引用表达式。
                plan.projections.push_back(bindExpression(reference, bindScope, context));
            }
            // 补齐结束。
        }
        // 投影补齐判断结束。
        for (const auto& item : statement.orderBy) {
        // 逐个排序键处理。
            const auto resolved = catalog::resolveOrder(statement, item, bindScope);
            // 先把"别名形式"的排序键还原成它真正指向的表达式。
            const auto bound = bindExpression(*resolved, bindScope, context);
            const auto identity = expressionIdentity(bound);
            // 算它的结构身份，用来在投影列表里找同款表达式。
            std::size_t index = 0;
            // 查找游标。
            while (index < plan.projections.size() && expressionIdentity(plan.projections[index]) != identity) ++index;
            // 线性查找：投影里已经有相同表达式就复用它。
            if (index == plan.projections.size()) {
            // 没找到，说明排序键不在投影里。
                if (statement.distinct) throw MiniSqlError(ErrorCode::Semantic, "DISTINCT ORDER BY must match a projected expression", item.expression->location);
                // DISTINCT 查询里排序键必须在投影中，否则去重后的结果无法定义排序，报错。
                plan.projections.push_back(bound);
                // 其它情况把排序键补成一条额外投影。
                plan.output.push_back({"_sort_" + std::to_string(sortKeys.size()), bound.at("type").get<std::string>(), static_cast<std::size_t>(-1), bound.value("nullable", true)});
                // 并补一列输出（名字形如 _sort_0），它是内部列，最后会被裁掉。
            }
            // 补投影结束。
            sortKeys.push_back({{"index", index}, {"descending", item.descending}, {"nullsFirst", item.nullsFirst.value_or(item.descending)}});
            // 记录排序键：投影下标、是否降序，以及 NULL 排前还是排后
            // （用户没显式写时按习惯跟随升降序）。
        }
        // 排序键处理结束。
    }
    // ORDER BY 处理结束。
    if (aggregated) {
    // 聚合查询：把投影改写成"聚合算子 + 上层投影"。
        lowerAggregate(plan, statement, bindScope, context);
        std::copy_n(plan.output.begin(), visibleOutput.size(), visibleOutput.begin());
        // 改写会调整输出列，这里用之前备份的可见列把它还原，保证对外列不变。
    }
    // 聚合处理结束。
    if (statement.kind == "Select" && statement.distinct) {
    // DISTINCT：在计划之上再套一层去重算子。
        LogicalPlan distinct;
        // 新建去重算子。
        distinct.kind = "Distinct";
        // 种类是 Distinct。
        distinct.table = plan.table;
        // 表名沿用。
        distinct.output = plan.output;
        // 输出列不变（去重不改变结果结构）。
        distinct.children.push_back(std::move(plan));
        // 当前计划成为它的输入。
        plan = std::move(distinct);
        // 去重算子成为当前计划。
    }
    // DISTINCT 处理结束。
    if (!sortKeys.empty()) {
    // 有排序键就再套一层排序算子。
        LogicalPlan sorted;
        // 新建排序算子。
        sorted.kind = "Sort";
        // 种类是 Sort。
        sorted.table = plan.table;
        // 表名沿用。
        sorted.output = visibleOutput;
        // 输出列用"用户可见列"，把前面为排序补的内部列排除在外。
        sorted.sortKeys = std::move(sortKeys);
        // 排序键搬进来。
        sorted.children.push_back(std::move(plan));
        // 当前计划成为输入。
        plan = std::move(sorted);
        // 排序算子成为当前计划。
    }
    // 排序处理结束。
    if (statement.kind == "Select" && (statement.limit || statement.offset)) {
    // 写了 LIMIT 或 OFFSET 时加一层限制算子。
        LogicalPlan limit;
        // 新建限制算子。
        limit.kind = "Limit";
        // 种类是 Limit。
        limit.table = plan.table;
        // 表名沿用。
        limit.output = plan.output;
        // 输出列不变。
        limit.limit = statement.limit;
        // 条数（未写 LIMIT 时为空，表示不限制条数但有偏移）。
        limit.offset = statement.offset;
        // 偏移量。
        limit.children.push_back(std::move(plan));
        // 当前计划成为输入。
        plan = std::move(limit);
        // 限制算子成为当前计划。
    }
    // 分页处理结束。
    return plan;
    // 返回构建好的计划。
}
}

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
// 编译入口：把一批语句逐个翻译成计划，并在过程中维护"编译期目录快照"。
                                      const catalog::Catalog& catalog) {
                                      // 传入的目录是只读的初始状态。
    auto snapshot = catalog;
    // 目录快照：第 i 条语句编译时看到的就是前面语句生效后的目录。
    std::optional<catalog::Catalog> transactionCatalog;
    // 事务开始时的目录快照，用于整体回滚恢复。
    std::map<std::string, catalog::Catalog> savepointCatalogs;
    // 保存点名字 → 当时的目录快照。
    std::vector<LogicalPlan> plans;
    // 结果计划列表。
    for (const auto& statement : statements) {
    // 按书写顺序逐条编译。
        if (statement.kind == "Begin") { transactionCatalog = snapshot; savepointCatalogs.clear(); }
        // 开始事务：记下起点快照，并清掉旧的保存点。
        else if (statement.kind == "Savepoint" && transactionCatalog) savepointCatalogs[canonical(statement.savepointName)] = snapshot;
        // 建保存点：把当前快照按名字存起来（只在事务内有效）。
        else if (statement.kind == "ReleaseSavepoint") savepointCatalogs.erase(canonical(statement.savepointName));
        // 释放保存点：删掉记录即可。
        else if (statement.kind == "RollbackTo" && transactionCatalog) {
        // 回滚到保存点。
            const auto found = savepointCatalogs.find(canonical(statement.savepointName));
            // 查找该保存点。
            if (found == savepointCatalogs.end()) invalid("savepoint does not exist: " + statement.savepointName);
            // 不存在就报内部错误（语义层本应先拦住）。
            snapshot = found->second;
            // 目录快照回退到保存点时的状态。
        }
        // 回滚到保存点处理结束。
        else if (statement.kind == "Rollback" && transactionCatalog) { snapshot = *transactionCatalog;transactionCatalog.reset();savepointCatalogs.clear(); }
        // 整体回滚：恢复到事务起点，并结束事务状态。
        else if (statement.kind == "Commit") { transactionCatalog.reset();savepointCatalogs.clear(); }
        // 提交：只是清掉事务与保存点记录，快照保持当前值。
        snapshot = catalog::compileSnapshot({statement}, snapshot);
        // 在快照上预演这条语句（建表等会真的改快照），得到"执行后"的目录。
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
    // 语句遍历结束。
    return plans;
    // 返回全部计划。
}

nlohmann::json serializePlans(const std::vector<LogicalPlan>& plans) {
// 把计划树序列化成"扁平行表"：每个节点一行，用 id/parent/children 表达树结构。
// 这样做的好处是执行层可以按行流式处理，也便于前端排版展示。
    nlohmann::json rows = nlohmann::json::array();
    // 结果行表。
    std::size_t nextId = 0;
    // 节点编号分配器。
    std::function<std::size_t(const LogicalPlan&, int, std::size_t, std::size_t)> visit;
    // 递归访问函数：参数依次是节点、父节点编号、深度、所属语句下标。
    visit = [&](const LogicalPlan& plan, int parent, std::size_t depth, std::size_t statementIndex) {
    // 处理一个节点。
        const auto id = nextId++;
        // 给它分配编号（先序遍历顺序）。
        const auto rowIndex = rows.size();
        // 这一行在行表里的位置，后面要按下标回填字段。
        nlohmann::json output = nlohmann::json::array();
        // 输出列的 JSON 数组。
        for (const auto& column : plan.output) {
        // 逐列序列化。
            output.push_back({{"name", column.name}, {"type", column.type}, {"columnId", column.columnId}, {"binding", column.binding}, {"relation", column.relation}, {"expressionId", column.expression}, {"nullable", column.nullable},
                {"defaultValue", column.defaultValue ? nlohmann::json(*column.defaultValue) : nlohmann::json(nullptr)}, {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", serializeReference(column.references)}});
                // 默认值（没有就 null）、主键/唯一标记与列级外键。
        }
        // 输出列序列化结束。
        const auto endLine = plan.sourceSpan.endLine ? plan.sourceSpan.endLine : plan.sourceSpan.line;
        const auto endColumn = plan.sourceSpan.endColumn ? plan.sourceSpan.endColumn : plan.sourceSpan.column + (plan.sourceSpan.line ? 1 : 0);
        rows.push_back({{"id", id}, {"nodeId", id}, {"parent", parent}, {"depth", depth}, {"statementIndex", statementIndex},
                        {"kind", plan.kind}, {"detail", plan.kind + " " + plan.table}, {"table", plan.table},
                        // 以及节点种类、一句人类可读的说明、表名，
                        {"sourceSpan", {{"start", {{"line", plan.sourceSpan.line}, {"column", plan.sourceSpan.column}}},
                                        {"end", {{"line", endLine}, {"column", endColumn}}}}},
                        {"catalogFingerprint", plan.catalogFingerprint},
                        {"optimizerDecision", plan.optimizerDecision},
                        {"indexName", plan.indexName}, {"savepointName", plan.savepointName}, {"subqueryJoinKind", plan.subqueryJoinKind}, {"uniqueIndex", plan.uniqueIndex}, {"indexColumns", plan.indexColumns}, {"indexValues", plan.indexValues}, {"indexRangeOperator", plan.indexRangeOperator}, {"indexRangeValue", plan.indexRangeValue},
                        // 索引与事务相关字段：索引名、保存点名、子查询连接种类、是否唯一索引、索引列、索引值、范围运算符与范围值，
                        {"output", output}, {"outputSchema", output}, {"preservesRowId", plan.preservesRowId},
                        {"predicate", plan.predicate}, {"values", plan.values}, {"insertExpressions", plan.insertExpressions}, {"insertRows", plan.insertRows},
                        // 谓词、字面量值、INSERT 表达式与多行数据，
                        {"columnMapping", plan.columnMapping}, {"projections", plan.projections}, {"children", nlohmann::json::array()}});
                        // 列映射、投影以及孩子编号列表（先占位，下面递归填充）。
        rows[rowIndex]["limit"] = plan.limit ? nlohmann::json(std::to_string(*plan.limit)) : nlohmann::json(nullptr);
        // LIMIT 转成字符串写回，避免大整数被 JSON number 精度截断。
        rows[rowIndex]["offset"] = std::to_string(plan.offset);
        // OFFSET 同理。
        rows[rowIndex]["sortKeys"] = plan.sortKeys;
        // 排序键。
        rows[rowIndex]["groupKeys"] = plan.groupKeys;
        // 分组键。
        rows[rowIndex]["aggregates"] = plan.aggregates;
        // 聚合调用列表。
        rows[rowIndex]["keys"] = serializeKeys(plan.keys);
        // 键约束（复用公共序列化函数保证格式一致）。
        rows[rowIndex]["checks"] = plan.checks;
        // CHECK 表达式。
        rows[rowIndex]["checkDefinitions"] = plan.checkDefinitions;
        // CHECK 的约束名与目标绑定。
        rows[rowIndex]["foreignKeys"] = serializeForeignKeys(plan.foreignKeys);
        // 外键。
        rows[rowIndex]["constraintNames"] = serializeConstraintNames(plan.constraintNames);
        // 约束命名绑定。
        for (const auto& child : plan.children) {
        // 递归处理每个孩子。
            const auto childId = visit(child, static_cast<int>(id), depth + 1, statementIndex);
            // 深度加一，父编号是当前节点。
            rows[rowIndex]["children"].push_back(childId);
            // 把孩子编号写回当前行的 children 列表。
        }
        // 孩子处理结束。
        return id;
        // 返回本节点编号。
    };
    // visit 定义结束。
    for (std::size_t i = 0; i < plans.size(); ++i) visit(plans[i], -1, 0, i);
    // 顶层计划的父编号记为 -1，深度从 0 开始。
    return rows;
    // 返回行表。
}

std::vector<LogicalPlan> deserializePlans(const nlohmann::json& document) {
// 反序列化入口：把 serializePlans 产出的扁平行表还原成计划树。
// 这是执行层侧的安全边界，所有外部输入都要严格校验。
    const auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized logical plan"); };
    // 局部工具：任何校验失败都抛同一条存储错误，不泄露细节。
    nlohmann::json rows;
    // 待处理的行表。
    if (document.is_array()) rows = document;
    // 形态一：直接就是行数组。
    else if (document.is_object() && document.value("schemaVersion", 0u) == PLAN_SCHEMA_VERSION && document.value("planKind", "") == "logical" && document.contains("plans"))
        // 形态二：带版本号的文档外壳，版本与种类都必须吻合。
        rows = document.at("plans");
        // 取出里面的行表。
    else invalid();
    // 其它形态一律判为非法。
    if (!rows.is_array() || rows.size() > 65536) invalid();
    // 必须是数组，并且节点数不超过 65536，防止超大文档耗尽内存。

    struct Pending {
    // 反序列化过程中的中间结构。
        LogicalPlan plan;
        // 已经还原出的节点。
        std::int64_t parent = -1;
        // 父节点编号，顶层为 -1。
        std::size_t statementIndex = 0;
        // 属于第几条语句。
        std::vector<std::size_t> children;
        // 孩子编号列表。
    };
    std::vector<Pending> pending;
    // 全部中间结构。
    pending.reserve(rows.size());
    // 按行数预留。
    std::function<void(const nlohmann::json&, std::size_t)> validateExpression;
    // 表达式校验函数（递归检查节点种类与结构）。
    validateExpression = [&](const nlohmann::json& expression, std::size_t depth) {
    // 校验一个表达式节点。
        if (depth > 256 || !expression.is_object()) invalid();
        // 过深或不是对象都判为非法。
        const auto kind = expression.value("kind", "");
        // 取节点种类。
        if (kind != "Literal" && kind != "Identifier" && kind != "Cast" &&
            // 只允许这几种节点：
            kind != "Unary" && kind != "Binary" && kind != "AggregateExpr") invalid();
            // 字面量、列引用、类型转换、一元、二元、聚合。其它一律拒绝。
        if (expression.contains("left") && !expression.at("left").is_null()) validateExpression(expression.at("left"), depth + 1);
        // 递归校验左子树。
        if (expression.contains("right") && !expression.at("right").is_null()) validateExpression(expression.at("right"), depth + 1);
        // 递归校验右子树。
    };
    // validateExpression 定义结束。

    for (std::size_t index = 0; index < rows.size(); ++index) {
    // 逐行还原节点。
        const auto& row = rows.at(index);
        // 当前行。
        if (!row.is_object() || !row.contains("id") || !row.at("id").is_number_unsigned() || row.at("id").get<std::size_t>() != index ||
            // 必须是对象，且 id 必须是非负整数并且等于它的行下标
            !row.contains("parent") || !row.at("parent").is_number_integer() || !row.contains("depth") || !row.at("depth").is_number_unsigned() ||
            // 还要有整数 parent 与非负整数 depth
            !row.contains("statementIndex") || !row.at("statementIndex").is_number_unsigned() || !row.contains("kind") || !row.at("kind").is_string() ||
            // 以及非负整数 statementIndex 与字符串 kind
            !row.contains("table") || !row.at("table").is_string() || !row.contains("output") || !row.at("output").is_array() ||
            // 字符串 table 与数组 output
            !row.contains("children") || !row.at("children").is_array() || !row.contains("columnMapping") || !row.at("columnMapping").is_array() ||
            // 数组 children 与数组 columnMapping
            !row.contains("preservesRowId") || !row.at("preservesRowId").is_boolean()) invalid();
            // 以及布尔 preservesRowId；任何一项不符都拒绝整份文档。
        Pending item;
        // 开始组装中间结构。
        item.parent = row.at("parent").get<std::int64_t>();
        // 还原父编号。
        if (item.parent < -1 || item.parent >= static_cast<std::int64_t>(rows.size())) invalid();
        // 父编号必须在 [-1, 行数) 范围内。
        item.statementIndex = row.at("statementIndex").get<std::size_t>();
        // 还原所属语句下标。
        item.plan.kind = row.at("kind").get<std::string>();
        // 还原节点种类。
        item.plan.table = row.at("table").get<std::string>();
        // 还原表名。
        item.plan.preservesRowId = row.at("preservesRowId").get<bool>();
        // 还原行号保持性。
        item.plan.optimizerDecision = row.value("optimizerDecision", nlohmann::json(nullptr));
        if (row.contains("sourceSpan")) {
            const auto& span = row.at("sourceSpan");
            if (!span.is_object() || !span.contains("start") || !span.contains("end") ||
                !span.at("start").is_object() || !span.at("end").is_object()) invalid();
            item.plan.sourceSpan = {span.at("start").value("line", std::size_t{0}), span.at("start").value("column", std::size_t{0}),
                                    span.at("end").value("line", std::size_t{0}), span.at("end").value("column", std::size_t{0})};
        }
    item.plan.indexName = row.value("indexName", std::string{});
    // 还原索引名（缺字段按空串）。
    item.plan.savepointName = row.value("savepointName", std::string{});
    // 还原保存点名。
    item.plan.subqueryJoinKind = row.value("subqueryJoinKind", std::string{});        item.plan.catalogFingerprint = row.value("catalogFingerprint", std::string{});        item.plan.uniqueIndex = row.value("uniqueIndex", false);
        item.plan.indexColumns = row.value("indexColumns", std::vector<std::string>{});
        // 还原索引列列表。
        item.plan.indexValues = row.value("indexValues", nlohmann::json::array());
        // 还原索引等值查找值。
        item.plan.indexRangeOperator = row.value("indexRangeOperator", std::string{});
        // 还原索引范围运算符。
        item.plan.indexRangeValue = row.value("indexRangeValue", nlohmann::json(nullptr));
        // 还原索引范围边界值。
        for (const auto& column : row.at("output")) {
        // 逐列还原输出列定义。
            if (!column.is_object() || !column.contains("name") || !column.at("name").is_string() || !column.contains("type") || !column.at("type").is_string() ||
                // 必须是对象，且有字符串 name 与 type，
                !column.contains("columnId") || !column.at("columnId").is_number_unsigned() || !column.contains("nullable") || !column.at("nullable").is_boolean()) invalid();
                // 还要有非负整数 columnId 与布尔 nullable，否则整份文档作废。
            PlanColumn value{column.at("name").get<std::string>(), column.at("type").get<std::string>(), column.at("columnId").get<std::size_t>(), column.at("nullable").get<bool>()};
            // 还原名称、类型、列编号与可空性。
            // X25：同 major 内的宽松读取——旧文档没有 binding/relation，缺失即无身份。
            value.binding = column.value("binding", std::uint32_t{0});
            value.relation = column.value("relation", std::uint32_t{0});
            value.expression = column.value("expressionId", std::uint32_t{0});
            if (column.contains("defaultValue") && !column.at("defaultValue").is_null()) value.defaultValue = column.at("defaultValue").get<std::string>();
            // 默认值可选，非 null 时才还原。
            value.primaryKey = column.value("primaryKey", false);
            // 还原主键标记。
            value.unique = column.value("unique", false);
            // 还原唯一标记。
            if (column.contains("references") && !column.at("references").is_null()) {
            // 列级外键可选。
                if (!column.at("references").is_object()) invalid();
                // 有值就必须是对象。
                value.references = std::make_pair(column.at("references").at("table").get<std::string>(), column.at("references").at("column").get<std::string>());
                // 还原成"父表名，父列名"。
            }
            // 外键处理结束。
            item.plan.output.push_back(std::move(value));
            // 收进该节点的输出列列表。
        }
        // 输出列处理结束。
        for (const auto& child : row.at("children")) {
        // 逐个子节点编号处理。
            if (!child.is_number_unsigned() || child.get<std::size_t>() >= rows.size()) invalid();
            // 必须是非负整数，并且指向真实存在的行。
            item.children.push_back(child.get<std::size_t>());
            // 记下孩子编号，稍后再递归建树。
        }
        // 孩子处理结束。
        for (const auto& value : row.at("columnMapping")) {
        // 列映射列表。
            if (!value.is_number_unsigned()) invalid();
            // 每个元素都必须是非负整数。
            item.plan.columnMapping.push_back(value.get<std::size_t>());
            // 逐个还原。
        }
        // 列映射处理结束。
        if (row.contains("limit") && !row.at("limit").is_null()) {
        // LIMIT 可选。
            if (!row.at("limit").is_string()) invalid();
            // 序列化时约定用字符串承载大整数，这里必须也是字符串。
            std::uint64_t limit{};
            const auto text = row.at("limit").get<std::string>();
            // 取出文本。
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
            // 文本转无符号整数。
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
            // 溢出或没读完都判为非法。
            item.plan.limit = limit;
            // 还原 LIMIT（这里才置成有值状态，与"没写 LIMIT"区分开）。
        }
        // LIMIT 处理结束。
        if (row.contains("offset")) {
        // OFFSET 可选。
            if (!row.at("offset").is_string()) invalid();
            // 同样要求字符串形式。
            const auto text = row.at("offset").get<std::string>();
            // 取出文本。
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), item.plan.offset);
            // 直接解析到目标字段。
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
            // 溢出或不完整都非法。
        }
        // OFFSET 处理结束。
        item.plan.predicate = row.value("predicate", nlohmann::json(nullptr));
        // 还原谓词（缺字段时按 null，表示没有条件）。
        item.plan.values = row.value("values", nlohmann::json::array());
        // 还原字面量值列表。
        item.plan.insertExpressions = row.value("insertExpressions", nlohmann::json::array());
        // 还原 INSERT 表达式列表。
        item.plan.insertRows = row.value("insertRows", nlohmann::json::array());
        // 还原 INSERT 多行数据。
        item.plan.projections = row.value("projections", nlohmann::json::array());
        // 还原投影表达式。
        item.plan.sortKeys = row.value("sortKeys", nlohmann::json::array());
        // 还原排序键。
        item.plan.groupKeys = row.value("groupKeys", nlohmann::json::array());
        // 还原分组键。
        item.plan.aggregates = row.value("aggregates", nlohmann::json::array());
        // 还原聚合调用。
        item.plan.checks = row.value("checks", nlohmann::json::array());
        // 还原 CHECK 表达式。
        item.plan.checkDefinitions = row.value("checkDefinitions", nlohmann::json::array());
        // 还原 CHECK 的约束命名定义。
        if (!item.plan.predicate.is_null()) validateExpression(item.plan.predicate, 0);
        // 谓词非空就要校验它的结构。
        for (const auto& expression : item.plan.projections) validateExpression(expression, 0);
        // 投影逐个校验。
        for (const auto& expression : item.plan.groupKeys) validateExpression(expression, 0);
        // 分组键逐个校验。
        for (const auto& aggregate : item.plan.aggregates) {
        // 聚合调用校验。
            if (!aggregate.is_object() || !aggregate.contains("argument")) invalid();
            // 必须是对象且带 argument 字段。
            if (!aggregate.at("argument").is_null()) validateExpression(aggregate.at("argument"), 0);
            // 参数非空（COUNT(*) 就是 null）时校验表达式结构。
        }
        // 聚合校验结束。
        for (const auto& expression : item.plan.insertExpressions) if (!expression.is_null()) validateExpression(expression, 0);
        // INSERT 表达式逐个校验。
        for (const auto& inserted : item.plan.insertRows) {
        // 多行插入逐行校验。
            if (!inserted.is_object() || !inserted.contains("expressions")) invalid();
            // 每行必须是对象且带 expressions。
            for (const auto& expression : inserted.at("expressions")) if (!expression.is_null()) validateExpression(expression, 0);
            // 行内表达式逐个校验。
        }
        // 多行校验结束。
        if (row.contains("keys")) for (const auto& key : row.at("keys")) item.plan.keys.push_back({key.at("primary").get<bool>(), key.at("columns").get<std::vector<std::string>>()});
        // 还原键约束（是否主键 + 列名列表）。
        if (row.contains("foreignKeys")) for (const auto& key : row.at("foreignKeys")) item.plan.foreignKeys.push_back({key.at("columns").get<std::vector<std::string>>(), key.at("table").get<std::string>(), key.at("referencedColumns").get<std::vector<std::string>>()});
        // 还原表级外键（本表列、父表名、父表列）。
        if (row.contains("constraintNames")) for (const auto& binding : row.at("constraintNames")) item.plan.constraintNames.push_back({binding.at("name").get<std::string>(), binding.at("kind").get<std::string>(), binding.at("index").get<std::size_t>()});
        // 还原约束命名绑定（名字、类别、目标下标）。
        pending.push_back(std::move(item));
        // 该行还原完毕，收进待建树列表。
    }
    // 行遍历结束。
    std::vector<bool> attached(rows.size(), false);
    // 记录每个节点是否已经被挂到树上，用于最后检查有没有孤立节点。
    std::function<LogicalPlan(std::size_t, std::size_t)> buildTree;
    // 递归建树函数。
    buildTree = [&](std::size_t index, std::size_t depth) -> LogicalPlan {
    // 从第 index 个节点开始建子树。
        if (depth > 256 || index >= pending.size() || attached[index]) invalid();
        // 深度过深、下标越界或重复挂载都判为非法，顺带防止环导致无限递归。
        attached[index] = true;
        // 标记已挂载。
        auto plan = pending[index].plan;
        // 复制出该节点。
        for (const auto child : pending[index].children) {
        // 逐个孩子。
            if (pending[child].parent != static_cast<std::int64_t>(index)) invalid();
            // 孩子记录的父编号必须确实指向当前节点，否则父子关系不自洽。
            plan.children.push_back(buildTree(child, depth + 1));
            // 递归建子树后挂上。
        }
        // 孩子处理结束。
        return plan;
        // 返回建好的子树。
    };
    // buildTree 定义结束。
    std::vector<LogicalPlan> result;
    // 结果计划列表。
    for (std::size_t index = 0; index < pending.size(); ++index) if (pending[index].parent == -1) result.push_back(buildTree(index, 0));
    // 父编号为 -1 的就是顶层计划，逐个建树。
    if (std::any_of(attached.begin(), attached.end(), [](bool value) { return !value; })) invalid();
    // 还有节点没被挂上，说明文档里有孤立节点，判为非法。
    return result;
    // 返回还原出的计划列表。
}
}
