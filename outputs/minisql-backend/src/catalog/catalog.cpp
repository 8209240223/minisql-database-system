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
// 统一转小写，作为大小写不敏感比较的规范形式。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // 逐字符转小写；入参用 unsigned char 避免负值传入未定义。
    return value;
    // 返回规范化后的字符串。
}

[[noreturn]] void fail(const std::string& message, SourceLocation location) {
// 语义错误的统一出口，位置信息用于前端定位。
    throw MiniSqlError(ErrorCode::Semantic, message, location);
}
// 错误消息里按 SQL 写法展示类型名：int -> INT、varchar(40) -> VARCHAR(40)。
// 错误消息里按 SQL 写法展示类型名：int -> INT、varchar(40) -> VARCHAR(40)。
std::string typeLabel(std::string value) {
// 把内部类型名转成大写形式，用于错误提示。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    // 逐字符转大写。
    return value;
}
bool assignable(const std::string& source, const std::string& target) {
// 判断源类型能否赋给目标类型，等价于插入时的类型兼容规则。
    if (stringType(source) && stringType(target)) return true;
    // 字符串之间互相兼容，长度在写入时另行校验。
    if (source == "null" || source == target || (target == "bigint" && source == "int")) return true;
    // 空值可赋给任何列；同类型直接允许；INT 可放宽到 BIGINT。
    if (target == "float") return source == "int" || source == "bigint" || decimalType(source).has_value();
    // 目标是 FLOAT 时，整数与 DECIMAL 都能隐式转换。
    if (source == "float") return false;
    // 反向不允许：FLOAT 不能隐式变成整数或 DECIMAL。
    const auto to = decimalType(target), from = decimalType(source);
    // 解析两侧的 DECIMAL 参数。
    if (!to) return false;
    // 目标不是 DECIMAL 就到此为止。
    if (source == "int" || source == "bigint") return true;
    // 整数可以放进 DECIMAL。
    return from && from->scale <= to->scale && from->precision - from->scale <= to->precision - to->scale;
    // 小数位数与整数位数都不能超过目标列，否则会丢精度。
}

const Column& column(const Table& table, const std::string& name, SourceLocation location) {
// 按名字取列定义，找不到就报错。
    return table.columns[resolveColumnIndex(table, name, location)];
    // 先解析成下标，再取列。
}

std::string literalType(const std::string& raw, SourceLocation location) {
// 推断一个字面量的类型。
    if (const auto date = dateLiteralText(raw)) { (void)parseIsoDate(*date,location,ErrorCode::Semantic);return "date"; }
    // DATE '...' 形式：先解析校验，再返回 date。
    if (key(raw) == "null") return "null";
    // SQL 空值。
    if (key(raw) == "true" || key(raw) == "false") return "bool";
    // 布尔字面量。
    if (!raw.empty() && raw.front() == '\'') return "varchar";
    // 以单引号开头视为字符串字面量。
    if (raw.find_first_of("eE") != std::string::npos) { (void)parseFiniteFloat(raw, ErrorCode::Semantic, location);return "float"; }
    // 含 e/E 视为科学计数法，先按浮点解析校验。
    if (raw.find('.') != std::string::npos) return decimalLiteral(raw, location).type.name();
    // 含小数点视为 DECIMAL，并返回推断出的 decimal(p,s)。
    const char* start = raw.data();
    // 起始指针。
    const char* end = start + raw.size();
    // 结束指针。
    if (start != end && *start == '+') ++start;
    // 跳过可能的前导正号，因为 from_chars 不接受它。
    std::int64_t value{};
    const auto result = std::from_chars(start, end, value);
    // 按 64 位整数解析。
    if (result.ec != std::errc{} || result.ptr != end) {
    // 解析失败或没消费完，说明超出 int64 范围或格式不对。
        throw MiniSqlError(ErrorCode::IntegerOutOfRange,
                           "SEM_INTEGER_OUT_OF_RANGE: integer literal is outside INT64 range: " + raw,
                           location);
    }
    return value < INT32_MIN || value > INT32_MAX ? "bigint" : "int";
    // 按取值范围决定是 BIGINT 还是 INT。
}

// 第十七章 REQ-CORE-001：语义在任何窄化转换之前检查 INT 范围。
// 字面量能装进 BIGINT 但装不进 INT（-2147483648..2147483647）而目标列是 INT 时，
// 报告稳定符号错误码 SEM_INTEGER_OUT_OF_RANGE，而不是笼统的类型不匹配。
// BIGINT 目标仍按 EXT-SQL-003 扩展接受，这是本项目的超集行为。
void rejectNarrowedInteger(const std::string& raw, const std::string& sourceType,
                           const std::string& targetType, SourceLocation location) {
    if (sourceType != "bigint" || key(targetType) != "int") return;
    if (raw.empty() || raw.front() == '\'') return;                  // 字符串字面量不参与
    if (raw.find_first_of("eE.") != std::string::npos) return;       // 小数/浮点不参与
    throw MiniSqlError(ErrorCode::IntegerOutOfRange,
        "SEM_INTEGER_OUT_OF_RANGE: " + raw +
        " does not fit INT (-2147483648..2147483647); use BIGINT or an explicit CAST",
        location);
}

std::string expressionType(const sql::Expr& expression, const Table& table,
                           SourceLocation statementLocation, std::size_t depth = 0, bool allowAggregate = false) {
// 表达式类型推导：语义检查的核心，返回表达式结果的类型名。
    auto location = expression.location.line ? expression.location : statementLocation;
    // 表达式自带位置就用它，否则退回到语句位置。
    if (depth > 256) fail("Expression depth exceeded", location);
    // 递归深度保护，防止极深表达式把栈打爆。
    if (expression.kind == "Identifier") return key(column(table, expression.value, location).type);
    // 标识符：查列定义得到类型。
    if (expression.kind == "Literal") return literalType(expression.value, location);
    // 字面量：按字面量规则推断。
    if (expression.kind == "Exists") {
    // EXISTS 子查询。
        if (expression.subquerySql.empty()) fail("EXISTS requires a subquery", location);
        // 必须带子查询。
        return "bool";
        // EXISTS 的结果是布尔。
    }
    if (expression.kind == "ScalarSubquery") {
    // 标量子查询。
        if (expression.subquerySql.empty()) fail("Scalar subquery requires SQL", location);
        // 必须带子查询文本。
        return "null";
        // 具体类型要到执行期才知道，这里先按可赋给任意类型处理。
    }
    if (expression.kind == "InSubquery") {
    // IN 子查询。
        if (!expression.left || expression.subquerySql.empty()) fail("IN subquery requires a left operand and subquery", location);
        // 左右两边都要有。
        const auto operand = expressionType(*expression.left, table, statementLocation, depth + 1, false);
        // 推导左操作数类型。
        if (operand != "null" && operand != "int" && operand != "bigint" && operand != "float" && !stringType(operand) && !decimalType(operand))
            // 只允许标量类型参与 IN。
            fail("IN subquery operand must be a scalar value", location);
        return "bool";
        // 结果是布尔。
    }
    if (expression.kind == "AggregateExpr") {
    // 聚合函数。
        if (!allowAggregate) fail("Aggregate functions are not allowed here or nested inside another aggregate", location);
        // 聚合函数只能出现在允许的位置，且不能嵌套。
        if (!expression.left || expression.right) fail("Aggregate requires one argument", location);
        // 必须恰好一个参数。
        const auto name = key(expression.value);
        // 函数名规范化。
        if (name != "count" && name != "sum" && name != "avg" && name != "min" && name != "max")
        // 只支持这五个聚合函数。
            fail("Unknown aggregate function: " + expression.value, location);
        if (expression.left->kind == "Wildcard") {
        // COUNT(*) 形式。
            if (name != "count" || expression.left->value != "*") fail("Only COUNT accepts '*'", location);
            // 只有 COUNT 能接受星号。
            return "bigint";
            // 计数结果用 BIGINT。
        }
        const auto argument = expressionType(*expression.left, table, statementLocation, depth + 1, false);
        // 推导参数类型，参数里不允许再出现聚合。
        if (name == "count") return "bigint";
        // COUNT 结果固定为 BIGINT。
        if (name == "sum" || name == "avg") {
        // SUM 与 AVG 的结果类型取决于参数。
            if (const auto decimal = decimalType(argument)) return DecimalType{38, name == "sum" ? decimal->scale : std::max(6u, decimal->scale)}.name();
            // DECIMAL 参数：SUM 保持标度，AVG 至少保留六位小数，精度统一放宽到 38。
            if (argument == "float") return "float";
            // 浮点参数结果仍是浮点。
            if (argument != "int" && argument != "bigint" && argument != "null") fail("SUM/AVG require numeric arguments", location);
            // 其余类型一律拒绝。
            return name == "sum" ? "bigint" : "decimal(38,6)";
            // 整数参数：SUM 可能溢出 32 位所以给 BIGINT，AVG 给固定标度 DECIMAL。
        }
        return argument;
        // MIN/MAX 的结果类型与参数一致。
    }
    if (!expression.left) fail("Expression is missing its operand", location);
    // 走到这里至少要有左操作数。
    const auto left = expressionType(*expression.left, table, statementLocation, depth + 1, allowAggregate);
    // 递归推导左操作数类型。
    if (expression.kind == "Cast") {
    // CAST 表达式。
        const auto target = key(expression.value);
        // 目标类型。
        if (target == "date" || left == "date") {
        // 涉及 DATE 的转换要单独约束。
            if ((target == "date" && (left == "date" || stringType(left) || left == "null")) || (left == "date" && stringType(target))) return target;
            // 只允许 DATE 与字符串之间互转。
            fail("DATE CAST permits only DATE and VARCHAR", location);
        }
        if (target == "bool" || left == "bool") {
        // 涉及 BOOL 的转换同理。
            if ((target == "bool" && (left == "bool" || stringType(left) || left == "null")) || (left == "bool" && stringType(target))) return target;
            // 只允许 BOOL 与字符串互转。
            fail("BOOL CAST permits only BOOL and VARCHAR", location);
        }
        if (target != "int" && target != "bigint" && target != "float" && !stringType(target) && !decimalType(target)) fail("Unsupported or invalid CAST target: " + target, location);
        // 其余目标类型必须是数值或字符串。
        if (left != "int" && left != "bigint" && left != "float" && !stringType(left) && left != "null" && !decimalType(left)) fail("Unsupported CAST source: " + left, location);
        // 源类型同样受限。
        return target;
        // 通过校验后结果就是目标类型。
    }
    if (expression.kind == "Unary" && (expression.value == "IS NULL" || expression.value == "IS NOT NULL")) return "bool";
    // IS NULL 判定返回布尔。
    if (expression.kind == "Unary" && (expression.value == "+" || expression.value == "-")) {
    // 一元正负号。
        if (left == "float") return "float";
        // 浮点保持浮点。
        if (decimalType(left)) return left;
        // DECIMAL 保持自身精度标度。
        if (left != "int" && left != "bigint" && left != "null") fail("Unary arithmetic requires integer", location);
        // 其余类型拒绝。
        return left == "bigint" ? "bigint" : "int";
        // 整数保持宽度。
    }
    if (expression.kind == "Unary" && expression.value == "NOT") {
    // 逻辑非。
        if (left != "bool" && left != "null") fail("NOT requires a BOOL operand", location);
        // 操作数必须是布尔或空值。
        return "bool";
        // 结果是布尔。
    }
    if (!expression.right) fail("Expression is missing its right operand", location);
    // 二元运算必须有右操作数。
    const auto right = expressionType(*expression.right, table, statementLocation, depth + 1, allowAggregate);
    // 递归推导右操作数类型。
    const auto numeric = [](const std::string& type) { return type == "int" || type == "bigint" || type == "float" || decimalType(type).has_value(); };
    // 判断是不是数值类型的小工具。
    if (isArithmetic(expression.value)) {
    // 四则运算分支。
        if ((!numeric(left) && left != "null") || (!numeric(right) && right != "null"))
        // 两侧都必须是数值或空值。
            fail("operator '" + expression.value + "' cannot be applied to " + typeLabel(left) + " and " + typeLabel(right), location);
            // 错误信息里给出具体运算符与两侧类型。
        if (left == "float" || right == "float") {
        // 只要有一侧是浮点。
            if ((left != "float" && left != "null") || (right != "float" && right != "null"))
            // 混合数值类型必须显式 CAST。
                fail("FLOAT arithmetic requires explicit CAST for mixed numeric types", location);
            return "float";
            // 浮点运算结果还是浮点。
        }
        const auto a = decimalType(left), b = decimalType(right);
        // 检查是否有 DECIMAL 参与。
        if (a || b) return decimalArithmeticType(expression.value, a ? a->scale : 0, b ? b->scale : 0, location).name();
        // 按运算种类推导结果标度。
        return left == "bigint" || right == "bigint" ? "bigint" : "int";
        // 纯整数运算：只要有一侧是 BIGINT 结果就是 BIGINT。
    }
    if (expression.value == "AND" || expression.value == "OR") {
    // 逻辑运算分支。
        if ((left != "bool" && left != "null") || (right != "bool" && right != "null")) fail("AND/OR require BOOL operands", location);
        // 两侧都必须是布尔或空值。
    } else if (left == "float" || right == "float") {
    // 比较运算且涉及浮点。
        if ((left != "float" && left != "null") || (right != "float" && right != "null"))
        // 浮点与其他数值比较需要显式 CAST。
            fail("FLOAT comparison requires explicit CAST for mixed numeric types", location);
    } else if (left != right && left != "null" && right != "null" && !(numeric(left) && numeric(right)) && !(stringType(left) && stringType(right))) {
    // 其余比较：类型必须相同，或者都是数值，或者都是字符串。
        fail("Comparison operands have incompatible types: " + left + " and " + right, location);
    }
    return "bool";
    // 所有比较与逻辑运算的结果都是布尔。
}
bool hasAggregate(const std::shared_ptr<sql::Expr>& expression, std::size_t depth = 0) {
// 判断表达式树里是否含聚合函数。
    if (!expression) return false;
    // 空表达式视为没有。
    if (depth > 256) fail("Expression depth exceeded", expression->location);
    // 深度保护。
    return expression->kind == "AggregateExpr" || hasAggregate(expression->left, depth + 1) || hasAggregate(expression->right, depth + 1);
    // 自身是聚合，或者左右子树里有聚合。
}
nlohmann::json groupIdentity(const sql::Expr& expression, const Table& scope, std::size_t depth = 0) {
// 计算表达式的“分组身份”：结构相同且指向同一列的两个表达式会得到相同结果，用于判断是否满足 GROUP BY。
    if (depth > 256) fail("Expression depth exceeded", expression.location);
    // 深度保护。
    if (expression.kind == "Identifier")
    // 标识符直接用列下标表示身份，避免别名带来的差异。
        return {{"kind", "Identifier"}, {"columnId", resolveColumnIndex(scope, expression.value, expression.location)}};
    auto value = expression.value;
    // 取表达式的值文本。
    if (expression.kind != "Literal" || value.empty() || value.front() != '\'') value = key(value);
    // 非字符串字面量统一转小写，使 TRUE 与 true 视为同一个分组键。
    if (expression.kind == "Literal" && decimalType(literalType(value, expression.location))) {
    // DECIMAL 字面量要按规范化的数值比较。
        const auto decimal = decimalLiteral(value, expression.location);
        return {{"kind", "Literal"}, {"type", decimal.type.name()}, {"value", decimal.value}};
        // 用规范化后的文本作为身份。
    }
    if (expression.kind == "Literal" && literalType(value, expression.location) != "varchar" && !dateLiteralText(value) &&
        value != "null" && value != "true" && value != "false") {
        // 普通整数字面量统一解析成数值，避免 001 与 1 被当成不同键。
        const auto* begin = value.data();
        // 起始指针。
        if (!value.empty() && value.front() == '+') ++begin;
        // 跳过正号。
        std::int64_t number{};
        const auto parsed = std::from_chars(begin, value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) fail("Invalid integer group key", expression.location);
        // 解析失败说明字面量有问题。
        return {{"kind", "Literal"}, {"value", number}};
        // 用数值作为身份。
    }
    nlohmann::json result = {{"kind", expression.kind}, {"value", value}};
    // 其余表达式按类型与规范化文本作为身份。
    if (expression.left) result["left"] = groupIdentity(*expression.left, scope, depth + 1);
    // 递归处理左子树。
    if (expression.right) result["right"] = groupIdentity(*expression.right, scope, depth + 1);
    // 递归处理右子树。
    return result;
    // 返回结构化身份。
}
void requireGrouped(const sql::Expr& expression, const Table& scope,
                    const std::unordered_set<std::string>& keys, std::size_t depth = 0) {
// 聚合查询里检查：每个非聚合引用都必须是分组键的一员。
    if (depth > 256) fail("Expression depth exceeded", expression.location);
    // 深度保护。
    if (expression.kind == "AggregateExpr" || expression.kind == "Literal") return;
    // 聚合函数与字面量不受分组限制。
    if (keys.contains(groupIdentity(expression, scope).dump())) return;
    // 自身就是分组键则通过。
    if (expression.kind == "Identifier" || expression.kind == "Wildcard")
    // 走到这里说明是没被分组的列引用。
        fail("Column must be grouped or aggregated: " + expression.value, expression.location);
        // 按 SQL 语义拒绝。
    if (expression.left) requireGrouped(*expression.left, scope, keys, depth + 1);
    // 递归左子树。
    if (expression.right) requireGrouped(*expression.right, scope, keys, depth + 1);
    // 递归右子树。
}
}

SelectAnalysis analyzeSelect(const sql::Statement& statement, const Table& scope) {
// 对一条 SELECT 做语义体检：判定是不是聚合查询，并逐个校验投影、ORDER BY、HAVING。
    SelectAnalysis analysis;
    // 准备结果结构，稍后把推导出的列类型和分组键类型填进去。
    analysis.aggregated = !statement.groupBy.empty() || static_cast<bool>(statement.having);
    // 只要出现 GROUP BY 或 HAVING，就先把这条查询标记成聚合查询。
    for (const auto& item : statement.selectItems) analysis.aggregated = hasAggregate(item.expression) || analysis.aggregated;
    // 再看投影列表里有没有聚合函数，有的话同样是聚合查询。
    for (const auto& item : statement.orderBy) analysis.aggregated = hasAggregate(item.expression) || analysis.aggregated;
    // ORDER BY 里出现聚合函数也按聚合查询处理，避免后面按普通列放开分组检查。
    std::unordered_set<std::string> keys;
    // 收集分组键的规范形式字符串，供 requireGrouped 做成员判断。
    for (const auto& group : statement.groupBy) {
    // 逐个处理 GROUP BY 里的表达式。
        if (!group) fail("Missing GROUP BY expression", statement.location);
        // 解析器给了空指针说明语法树有洞，直接按语义错误报出。
        analysis.groupTypes.push_back(expressionType(*group, scope, statement.location));
        // 推导分组键的类型，生成计划时要用它做分组比较。
        keys.insert(groupIdentity(*group, scope).dump());
        // 把分组键结构化身份序列化成文本存进集合，后续按字符串比对。
    }
    for (const auto& item : statement.selectItems) {
    // 逐个校验投影项。
        if (!item.expression) fail("Missing projection expression", statement.location);
        // 投影项必须有表达式节点。
        if (item.expression->kind == "Wildcard") {
        // 分支一：这一项是 * 或 表名.* 这种通配展开。
            (void)resolveColumnName(scope, item.expression->value, item.expression->location);
            // 先校验通配前缀里的表名/别名确实存在于视野表，前缀非法就立刻报错。
            const auto dot = item.expression->value.find('.');
            // 找出 表名.列名 里的那个点，用来判断这句通配是不是带了限定前缀。
            for (const auto& source : scope.columns) {
            // 遍历视野表里所有列，把通配展开成一条条真实的列引用。
                const auto qualifier = source.qualifier.empty() ? scope.name : source.qualifier;
                // 每列用它的限定名（表名或别名）作为前缀；没有限定名就退回视野表名。
                if (dot != std::string::npos && key(qualifier) != key(item.expression->value.substr(0, dot))) continue;
                // 如果用户写的是 表名.*，只保留前缀匹配的那些列，其余表直接跳过。
                const sql::Expr reference{"Identifier", qualifier + "." + source.name, {}, {}, item.expression->location};
                // 临时造一个限定列名表达式，用同一套分组检查逻辑去看它合不合规。
                if (analysis.aggregated) requireGrouped(reference, scope, keys);
                // 聚合查询下，通配展开出来的每一列也都必须满足"被分组或被聚合"。
                analysis.projectionTypes.push_back(source.type);
                // 把这一列的声明类型记进投影类型表，供上层推导结果集元数据。
            }
        } else {
        // 分支二：普通表达式投影。
            analysis.projectionTypes.push_back(expressionType(*item.expression, scope, statement.location, 0, true));
            // 推导该项目类型；最后一个 true 表示允许里面出现聚合函数。
            if (analysis.aggregated) requireGrouped(*item.expression, scope, keys);
            // 聚合查询下再检查一次：表达式里凡是引用列的位置都不能是"裸列"。
        }
    }
    for (const auto& item : statement.orderBy) {
    // 逐个校验 ORDER BY 项。
        const auto expression = resolveOrder(statement, item, scope);
        // 先把 ORDER BY 里的别名还原成它真正指向的投影表达式。
        (void)expressionType(*expression, scope, statement.location, 0, true);
        // 推导排序键类型，顺便把里面引用错列的情况揪出来。
        if (analysis.aggregated) requireGrouped(*expression, scope, keys);
        // 聚合查询下排序键同样受分组规则约束。
    }
    if (statement.having) {
    // HAVING 出现时单独体检。
        const auto type = expressionType(*statement.having, scope, statement.location, 0, true);
        // 推导 HAVING 条件类型。
        if (type != "bool" && type != "null") fail("HAVING requires a BOOL expression", statement.having->location);
        // HAVING 是过滤条件，类型必须是布尔；NULL 类型放行以便容忍未知类型。
        requireGrouped(*statement.having, scope, keys);
        // HAVING 里引用的列也必须被分组或被聚合，否则语义不成立。
    }
    return analysis;
    // 返回推导结果，planner 会拿它决定是否需要插入聚合算子。
}

std::string resolveColumnName(const Table& table, const std::string& name, SourceLocation location) {
// 把 表名.列名 解析成裸列名，并校验限定前缀是否合法。
    const auto dot = name.find('.');
    // 找限定符与列名之间的分隔点。
    if (dot == std::string::npos) return name;
    // 没有点说明本来就是裸列名，原样返回。
    bool found = false;
    // 标记限定前缀是否能在视野表里找到对应来源。
    for (const auto& column : table.columns)
    // 遍历视野表所有列，用它们的来源限定名去比对前缀。
        if (key(column.qualifier.empty() ? table.name : column.qualifier) == key(name.substr(0, dot))) found = true;
        // 列的限定名与用户写的前缀（都转小写后）相等即认为匹配。
    if (!found || name.find('.', dot + 1) != std::string::npos)
    // 前缀找不到，或者名字里还有第二个点（如 a.b.c），都算非法限定名。
        fail("Unknown table qualifier: " + name.substr(0, dot), location);
        // 按语义错误报出，附带出错位置。
    return name.substr(dot + 1);
    // 校验通过，返回去掉前缀后的裸列名。
}
std::size_t resolveColumnIndex(const Table& table, const std::string& name, SourceLocation location) {
// 把列名解析成它在视野表里的下标，同时检测歧义与不存在两种情况。
    const auto plain = resolveColumnName(table, name, location);
    // 先剥掉可能的表名前缀，得到裸列名。
    const auto dot = name.find('.');
    // 记住用户有没有写限定前缀，后面匹配时要用。
    std::size_t match = table.columns.size();
    // 用"越界下标"当哨兵，表示目前一个候选都没找到。
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
    // 逐个扫描视野表的列。
        const auto& column = table.columns[i];
        // 取出当前列，便于下面反复访问字段。
        if (key(column.name) != key(plain)) continue;
        // 列名（大小写不敏感）对不上就跳过。
        if (dot != std::string::npos && key(column.qualifier.empty() ? table.name : column.qualifier) != key(name.substr(0, dot))) continue;
        // 用户写了前缀时，前缀也要对得上，否则这个同名列不属于该表。
        if (match != table.columns.size()) fail("Ambiguous column: " + name, location);
        // 已经有候选又冒出第二个候选，说明列名有歧义，直接报错。
        match = i;
        // 记下当前候选下标。
    }
    if (match == table.columns.size()) fail("Column '" + name + "' does not exist in table '" + table.name + "'", location);
    // 循环结束仍是哨兵值，说明这一列压根不存在。
    return match;
    // 返回唯一的列下标。
}
Table queryScope(const sql::Statement& statement, const Catalog& catalog, std::size_t joinCount) {
// 把主表与参与连接的右表合并成一张"视野表"，让后续校验统一按一张表来查列。
    const auto* first = catalog.find(statement.table);
    // 先按表名取出主表定义。
    if (!first) fail("Table does not exist: " + statement.table, statement.location);
    // 主表不存在就没法继续，按语义错误报出。
    Table scope{statement.tableAlias.empty() ? first->name : statement.tableAlias, first->columns};
    // 视图表沿用主表列，但对外名字换成别名（没写别名就用表名）。
    for (auto& column : scope.columns) column.qualifier = scope.name;
    // 给主表每一列打上限定名，这样 别名.列名 才能被解析。
    std::unordered_set<std::string> names{key(scope.name)};
    // 记录已经用掉的表名/别名，用来检测重复别名。
    for (std::size_t i = 0; i < std::min(joinCount, statement.joins.size()); ++i) {
    // 只合并需要参与本次校验的那些 JOIN。
        const auto& join = statement.joins[i];
        // 取出第 i 个 JOIN 子句。
        const auto* right = catalog.find(join.table);
        // 按表名取出被连接的右表定义。
        if (!right) fail("Table does not exist: " + join.table, statement.location);
        // 右表不存在同样直接报错。
        const auto qualifier = join.alias.empty() ? right->name : join.alias;
        // 右表对外使用的限定名，优先用别名。
        if (!names.insert(key(qualifier)).second) fail("Duplicate table alias: " + qualifier, statement.location);
        // 插入失败说明这个名字已经被用过，属于重复别名。
        if (join.right) for (auto& column : scope.columns) column.nullable = true;
        // RIGHT JOIN 时左表整侧都可能补 NULL，所以把左侧现有列全部标记为可空。
        for (auto column : right->columns) { column.qualifier = qualifier;if (join.left) column.nullable = true;scope.columns.push_back(std::move(column)); }
        // 逐列拷贝右表列：打上右表限定名；LEFT JOIN 时右表可能补 NULL，标记可空；然后并入视图表。
        if (!join.on) fail("JOIN ON requires a BOOL expression", statement.location);
        // JOIN 必须带 ON 条件。
        const auto type = expressionType(*join.on, scope, statement.location);
        // 用"已经合并了前面若干表"的视图表来推导 ON 条件类型。
        if (type != "bool" && type != "null")
        // ON 必须是布尔表达式；NULL 类型放行以容忍未知类型。
            fail("JOIN ON requires a BOOL expression", statement.location);
            // 类型不合法则报错。
    }
    return scope;
    // 返回合并完成后的视图表。
}
void Catalog::create(const sql::Statement& statement) {
// 执行 CREATE TABLE：把语法树里的建表信息校验并落成一份 Table 元数据。
    if (find(statement.table)) {
    // 同名表已经存在于内存目录里。
        throw MiniSqlError(ErrorCode::Catalog, "Table already exists: " + statement.table, statement.location);
        // 抛出目录级错误，调用方据此终止建表。
    }
    Table table{statement.table, {}};
    // 先造一个只有表名的空表结构，后面逐项往里填列、键、外键、检查。
    std::unordered_set<std::string> names;
    // 记录已出现的列名，用于查重。
    bool hasPrimaryKey = false;
    // 标记是否已经声明过主键，因为一张表只能有一个主键。
    for (const auto& definition : statement.columns) {
    // 逐列处理建表语句中的列定义。
        const auto declared = key(definition.type);
        // 把声明类型转小写，后续比较统一按小写。
        if (declared != "int" && declared != "bigint" && declared != "float" && !stringType(declared) && declared != "bool" && declared != "date" && !decimalType(declared)) fail("Unsupported or invalid column type", statement.location);
        // 类型白名单校验：不在支持列表里的类型一律拒绝，避免后续存储层遇到陌生类型。
        if (definition.primaryKey) {
        // 这一列被声明为列级主键。
            if (hasPrimaryKey) fail("Multiple PRIMARY KEY declarations", statement.location);
            // 已经有过主键，再出现就是语法允许但语义非法的重复主键。
            hasPrimaryKey = true;
            // 记录主键已声明。
            if (definition.nullable) fail("PRIMARY KEY must be NOT NULL", statement.location);
            // 主键必须非空，声明里写了 NULL 就报错。
        }
        if (!names.insert(key(definition.name)).second) {
        // 列名转小写后插入集合，插入失败说明重名。
            throw MiniSqlError(ErrorCode::Catalog, "Duplicate column: " + definition.name, statement.location);
            // 重复列名属于目录层错误。
        }
        if (definition.defaultValue) {
        // 该列写了 DEFAULT，需要逐项校验默认值合不合法。
            const auto tokens = sql::tokenize(*definition.defaultValue);
            // 把默认值文本重新词法切分，借 token 结构判断它是不是"一个完整字面量"。
            const bool unsignedLiteral = tokens.size() == 2 && (tokens[0].type == "INTEGER" || tokens[0].type == "DECIMAL" || tokens[0].type == "FLOAT" || tokens[0].type == "STRING" ||
                key(tokens[0].lexeme) == "null" || key(tokens[0].lexeme) == "true" || key(tokens[0].lexeme) == "false");
            // 形态一：一个字面量加一个结束标记，即无符号数字、字符串、NULL、TRUE/FALSE。
            const bool signedLiteral = tokens.size() == 3 && (tokens[0].lexeme == "+" || tokens[0].lexeme == "-") && (tokens[1].type == "INTEGER" || tokens[1].type == "DECIMAL" || tokens[1].type == "FLOAT");
            // 形态二：正负号加数字字面量。
            const bool dateLiteral = tokens.size() == 3 && key(tokens[0].lexeme) == "date" && tokens[1].type == "STRING";
            // 形态三：DATE 关键字加一个字符串字面量。
            if (!unsignedLiteral && !signedLiteral && !dateLiteral) fail("DEFAULT requires one literal", statement.location);
            // 三种形态都不满足，说明默认值不是单个字面量，拒绝。
            const auto type = literalType(*definition.defaultValue, statement.location);
            // 推导默认值的类型。
            rejectNarrowedInteger(*definition.defaultValue, type, declared, statement.location);
            if ((type == "null" && !definition.nullable) || !assignable(type, declared))
            // 两种情况非法：给 NOT NULL 列设 NULL 默认值；或者默认值类型无法赋给列类型。
                fail("DEFAULT type mismatch for column: " + definition.name, statement.location);
                // 按语义错误报出具体列名。
            if (const auto limit=varcharLength(declared);limit && type!="null")
            // 如果这列是变长字符串且默认值不是 NULL，还要看长度越界没有。
                validateVarchar(stringLiteralValue(*definition.defaultValue,statement.location),*limit,statement.location,ErrorCode::Semantic);
                // 取出默认值字符串的实际内容，按列声明长度做上限校验。
            if (const auto decimal = decimalType(declared); decimal && type != "null") {
            // 如果这列是定点数且默认值不是 NULL，还要校验精度标度是否放得下。
                try { (void)ExactDecimal::parse(*definition.defaultValue, decimal->precision, decimal->scale); }
                // 按列声明的 precision/scale 试着解析，解析成功即表示精度够用。
                catch (const MiniSqlError&) { fail("DEFAULT exceeds DECIMAL column precision", statement.location); }
                // 解析失败说明默认值超出精度，转成语义错误报出。
            }
        }
        table.columns.push_back({definition.name, key(definition.type), {}, definition.nullable, definition.defaultValue, definition.primaryKey, definition.unique, definition.references});
        // 把这一列加入表结构：限定名先留空（等查询时再打），其余字段直接沿用语法树。
    }
    for (const auto& constraint : statement.keys) {
    // 处理表级 PRIMARY KEY / UNIQUE 约束。
        if (constraint.columns.empty()) fail("Empty key constraint", statement.location);
        // 键约束必须至少指定一列。
        if (constraint.primary && hasPrimaryKey) fail("Multiple PRIMARY KEY declarations", statement.location);
        // 列级已经声明过主键，表级再来一个就冲突了。
        if (constraint.primary) hasPrimaryKey = true;
        // 记录主键已声明。
        sql::KeyConstraint bound;bound.primary = constraint.primary;
        // 准备绑定后的键约束，先复制"是不是主键"这个标志。
        std::unordered_set<std::size_t> seenColumns;
        // 记录本约束里已经用过的列下标，检测同一列被写两次。
        for (const auto& name : constraint.columns) {
        // 逐个解析约束引用的列名。
            const auto index = resolveColumnIndex(table, name, statement.location);
            // 把列名换成列下标；列不存在或有歧义会在这里报错。
            if (!seenColumns.insert(index).second) fail("Duplicate key column: " + name, statement.location);
            // 同一列在一条键约束里出现两次属于非法。
            auto& column = table.columns[index];
            // 取出被约束的列，主键场景下要就地修改它。
            if (constraint.primary) {
            // 这一条是主键约束。
                if (column.defaultValue && literalType(*column.defaultValue, statement.location) == "null") fail("PRIMARY KEY default cannot be NULL", statement.location);
                // 主键列不允许把默认值设成 NULL，否则每次插入都会破坏主键非空性。
                column.nullable = false;
                // 主键列强制改为非空。
            }
            bound.columns.push_back(column.name);
            // 记录约束列名（已经用真实列名规范化过）。
        }
        table.keys.push_back(std::move(bound));
        // 把这条键约束挂到表定义上。
    }
    const auto references = sql::allForeignKeys(statement);
    // 把列级和表级两种写法声明出来的外键汇总成一个列表。
    for (std::size_t r = 0; r < references.size(); ++r) {
    // 逐个校验外键。
        const auto& reference = references[r];
        // 取当前外键定义。
        if (reference.columns.empty() || reference.columns.size() != reference.referencedColumns.size())
        // 本表列与被引用列必须一一对应且非空。
            fail("Foreign key column counts must match and be nonempty", statement.location);
            // 数量对不上就报错。
        const auto* parent = key(reference.table) == key(table.name) ? &table : find(reference.table);
        // 定位被引用表；如果引用的就是自己，直接用当前正在构造的表。
        if (!parent) fail("Referenced table does not exist: " + reference.table, statement.location);
        // 被引用表不存在则拒绝建表。
        sql::ForeignKey bound;bound.table = parent->name;
        // 准备绑定后的外键，记录父表的规范名。
        std::unordered_set<std::size_t> childSeen, parentSeen;
        // 分别记录子表侧与父表侧已用过的列下标，检测重复列。
        std::vector<std::size_t> parentIndices;
        // 保存父表侧的列下标，稍后用来判断它是不是键。
        for (std::size_t i = 0; i < reference.columns.size(); ++i) {
        // 逐个处理外键中的列配对。
            const auto childIndex = resolveColumnIndex(table, reference.columns[i], statement.location);
            // 解析子表侧的列下标。
            const auto parentIndex = resolveColumnIndex(*parent, reference.referencedColumns[i], statement.location);
            // 解析父表侧的列下标。
            if (!childSeen.insert(childIndex).second || !parentSeen.insert(parentIndex).second)
            // 任何一侧出现重复列都非法。
                fail("Duplicate foreign key column", statement.location);
                // 报错并结束。
            if (key(table.columns[childIndex].type) != key(parent->columns[parentIndex].type) &&
                !(stringType(key(table.columns[childIndex].type)) && stringType(key(parent->columns[parentIndex].type))))
            // 两侧类型必须一致；唯一的例外是两边都是字符串类型，允许长度不同。
                fail("Foreign key types are incompatible", statement.location);
                // 类型不兼容则拒绝。
            bound.columns.push_back(table.columns[childIndex].name);
            // 记录子表侧列名。
            bound.referencedColumns.push_back(parent->columns[parentIndex].name);
            // 记录父表侧列名。
            parentIndices.push_back(parentIndex);
            // 记录父表侧列下标。
        }
        bool referencedKey = parentIndices.size() == 1 &&
            (parent->columns[parentIndices[0]].primaryKey || parent->columns[parentIndices[0]].unique);
        // 单列外键时，只要父列自己是主键或唯一列，就算引用了一个键。
        for (const auto& constraint : parent->keys) {
        // 继续检查父表上的表级键约束。
            if (constraint.columns.size() != parentIndices.size()) continue;
            // 列数不一致的约束不可能匹配，跳过。
            bool matches = true;
            // 假设这条约束与本次外键的父列顺序一致。
            for (std::size_t i = 0; i < parentIndices.size(); ++i)
            // 逐列比对名字与顺序。
                matches = matches && key(constraint.columns[i]) == key(bound.referencedColumns[i]);
                // 有一列对不上就说明不匹配。
            referencedKey = referencedKey || matches;
            // 任意一条约束匹配成功即可认定父列是键。
        }
        if (!referencedKey) fail("Referenced columns must match an ordered PRIMARY KEY or UNIQUE key", statement.location);
        // 父列既不是主键/唯一列，也不构成某条表级键，则外键不合法。
        if (r < statement.foreignKeys.size()) table.foreignKeys.push_back(std::move(bound));
        // 只把表级声明的那部分写回表定义，列级外键保存在列自身的 references 字段里。
    }
    for (const auto& check : statement.checks) {
    // 逐条校验 CHECK 约束表达式。
        const auto scope = Table{statement.table, table.columns, table.keys, {}};
        // 用"列已全部就位"的临时表作为视野，保证 CHECK 能引用任意一列。
        if (!check) fail("CHECK expression is missing", statement.location);
        // 表达式指针为空说明语法树有问题。
        const auto type = expressionType(*check, scope, statement.location);
        // 推导 CHECK 表达式类型。
        if (type != "bool" && type != "null") fail("CHECK must be a BOOL expression", statement.location);
        // CHECK 必须是布尔条件，否则无法在插入时判定真假。
        table.checks.push_back(sql::serializeExpression(check).dump());
        // 把表达式序列化成 JSON 文本存下来，执行期再做绑定求值。
    }
    std::unordered_set<std::string> constraintNames, namedTargets;
    // constraintNames 检测约束名重名；namedTargets 检测同一目标被起了两个名字。
    for (const auto& binding : statement.constraintNames) {
    // 逐个处理用户显式给出的 CONSTRAINT 名字绑定。
        const auto tokens = sql::tokenize(binding.name);
        // 把约束名切 token，确认它是一个合法标识符而不是表达式片段。
        if (tokens.size() != 2 || tokens[0].type != "IDENTIFIER" || tokens[0].lexeme != binding.name)
        // 合法形态应恰好是"一个标识符 + 结束标记"，且词素与原文完全一致。
            fail("Invalid constraint name", statement.location);
            // 不满足就拒绝。
        if (!constraintNames.insert(key(binding.name)).second) fail("Duplicate constraint name: " + binding.name, statement.location);
        // 约束名在表内必须唯一。
        if (!namedTargets.insert(binding.kind + ":" + std::to_string(binding.index)).second)
        // 同一类目标上的同一个下标被命名两次也不允许。
            fail("Constraint has multiple names", statement.location);
            // 报出"约束被起了多个名字"。
        bool valid = (binding.kind == "key" && binding.index < table.keys.size()) ||
            (binding.kind == "check" && binding.index < table.checks.size()) ||
            (binding.kind == "foreignKey" && binding.index < table.foreignKeys.size());
        // 先判断它指向的表级目标是否真实存在（键、CHECK、外键各自的下标范围）。
        if (binding.index < table.columns.size()) {
        // 再判断它是不是指向某一列的列级约束。
            const auto& target = table.columns[binding.index];
            // 取出目标列。
            valid = valid || (binding.kind == "primaryKey" && target.primaryKey) ||
                (binding.kind == "unique" && target.unique) || (binding.kind == "references" && target.references.has_value()) ||
                (binding.kind == "notNull" && !target.nullable);
            // 列级约束必须与该列真实拥有的属性一致，否则这个命名就是悬空的。
        }
        if (!valid) fail("Constraint name references an invalid target", statement.location);
        // 表级、列级都对不上，说明名字绑到了不存在的东西上。
        table.constraintNames.push_back(binding);
        // 记录这条合法的名字绑定，输出元数据时要写出去。
    }
    tables_.emplace(key(statement.table), std::move(table));
    // 全部校验通过，把表以规范化表名为键写入内存目录。
}

void Catalog::createIndex(const sql::Statement& statement) {
// 执行 CREATE INDEX：把索引名与列清单登记到目标表的元数据上。
    if (statement.kind != "CreateIndex" || statement.indexName.empty() || statement.indexColumns.empty())
    // 语句种类、索引名、索引列三者任一缺失都视为非法定义。
        fail("Invalid CREATE INDEX definition", statement.location);
        // 按语义错误报出。
    auto found = tables_.find(key(statement.table));
    // 在内存目录里按规范化表名查找目标表。
    if (found == tables_.end()) fail("Index table does not exist: " + statement.table, statement.location);
    // 表不存在就不能建索引。
    auto& table = found->second;
    // 取出表定义的可写引用。
    for (const auto& index : table.indexes)
    // 遍历该表已有索引，检查名字是否重复。
        if (key(index.name) == key(statement.indexName)) fail("Index already exists: " + statement.indexName, statement.location);
        // 同名索引重复创建属于错误。
    Index index;
    // 准备一条新的索引元数据。
    index.name = statement.indexName;
    // 记录索引名。
    index.unique = statement.uniqueIndex;
    // 记录它是不是唯一索引，唯一性检查在插入时要读这个标志。
    for (const auto& name : statement.indexColumns) {
    // 逐个解析索引列。
        const auto position = resolveColumnIndex(table, name, statement.location);
        // 列名换成列下标，顺带校验列存在且不歧义。
        index.columns.push_back(table.columns[position].name);
        // 存规范列名而不是用户原文，避免大小写差异导致后续比对失败。
    }
    table.indexes.push_back(std::move(index));
    // 把索引挂到表上。
}
void Catalog::dropIndex(const sql::Statement& statement) {
// 执行 DROP INDEX：在目录中删掉指定索引。
    if (statement.kind != "DropIndex" || statement.indexName.empty()) fail("Invalid DROP INDEX definition", statement.location);
    // 语句种类或索引名缺失都算非法。
    for (auto& [name, table] : tables_) {
    // 逐个表扫描；DROP INDEX 允许不写表名，所以要靠搜索定位。
        if (!statement.table.empty() && key(statement.table) != name) continue;
        // 用户若写了表名，就只在那一张表里找。
        const auto found = std::find_if(table.indexes.begin(), table.indexes.end(), [&](const Index& index) { return key(index.name) == key(statement.indexName); });
        // 在表的索引列表里按名字查找。
        if (found == table.indexes.end()) continue;
        // 这张表没有该索引，继续看下一张表。
        table.indexes.erase(found);
        // 找到即删除。
        return;
        // 一次只删一个索引，删完直接返回。
    }
    fail("Index does not exist: " + statement.indexName, statement.location);
    // 所有表都找遍还没找到，说明索引不存在。
}
std::string notNullConstraintSuffix(const Table& table, std::size_t index) {
// 生成"NOT NULL 违规"错误的补充文字，把用户起的约束名带回给调用方。
    auto suffix = sql::constraintSuffix(table.constraintNames, "notNull", index);
    // 先看这一列有没有显式命名的 NOT NULL 约束。
    if (!suffix.empty()) return suffix;
    // 有就直接用。
    suffix = sql::constraintSuffix(table.constraintNames, "primaryKey", index);
    // 再退一步，看它是不是被命名为某个主键约束的列。
    if (!suffix.empty()) return suffix;
    // 有就返回。
    for (std::size_t i = 0; i < table.keys.size(); ++i)
    // 继续扫描表级键约束。
        if (table.keys[i].primary) for (const auto& name : table.keys[i].columns)
        // 只看主键约束，并逐列比对。
            if (key(name) == key(table.columns.at(index).name)) return sql::constraintSuffix(table.constraintNames, "key", i);
            // 命中同名主键列就返回该键约束的名字后缀。
    return {};
    // 没有任何命名约束，返回空串，错误信息里就不追加后缀。
}

const Table* Catalog::find(const std::string& name) const {
// 按表名查表，大小写不敏感；找不到返回空指针而不是抛异常。
    auto it = tables_.find(key(name));
    // 用规范化表名在哈希表里定位。
    return it == tables_.end() ? nullptr : &it->second;
    // 未命中返回 nullptr，命中返回表定义的只读指针。
}

const Table* Catalog::findIndexTable(const std::string& indexName) const {
    for (const auto& [_, table] : tables_)
        if (std::any_of(table.indexes.begin(), table.indexes.end(), [&](const Index& index) {
                return key(index.name) == key(indexName);
            })) return &table;
    return nullptr;
}

std::string Catalog::schemaFingerprint() const {
    // 表遍历顺序取决于 unordered_map，必须先按名字排序才能得到稳定指纹。
    std::vector<const Table*> ordered;
    ordered.reserve(tables_.size());
    for (const auto& entry : tables_) ordered.push_back(&entry.second);
    std::sort(ordered.begin(), ordered.end(), [](const Table* left, const Table* right) {
        return key(left->name) < key(right->name);
    });
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](const std::string& text) {
        for (const unsigned char byte : text) { hash ^= byte; hash *= 0x100000001b3ULL; }
        hash ^= 0x1f; hash *= 0x100000001b3ULL;   // 字段分隔符，避免拼接歧义
    };
    for (const auto* table : ordered) {
        mix(key(table->name));
        for (const auto& column : table->columns) {
            mix(key(column.name));
            mix(key(column.type));
            mix(column.nullable ? "1" : "0");
            mix(column.primaryKey ? "1" : "0");
            mix(column.unique ? "1" : "0");
        }
        mix("|");   // 表边界
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[static_cast<std::size_t>(index)] = digits[hash & 0xfULL];
        hash >>= 4;
    }
    return out;
}

std::vector<std::string> insertColumns(const sql::Statement& statement, const Table& table) {
// 算出 INSERT 语句实际要写入哪些列。
    if (statement.defaultValues) {
    // 语句写的是 INSERT ... DEFAULT VALUES。
        if (!statement.names.empty() || !statement.values.empty() || !statement.valueExpressions.empty())
        // 这种写法不允许再额外指定列名或值。
            fail("DEFAULT VALUES cannot specify columns or values", statement.location);
            // 冲突就报错。
        return {};
        // 返回空列表，交给执行层按各列默认值填充。
    }
    if (!statement.names.empty()) return statement.names;
    // 用户显式写了列清单，直接沿用（顺序就是写值的顺序）。
    std::vector<std::string> names;
    // 否则要按建表顺序补齐全部列。
    for (const auto& column : table.columns) names.push_back(column.name);
    // 逐个拷贝列名，保证列清单与表定义顺序一致。
    return names;
    // 返回最终列清单。
}
void validate(const std::vector<sql::Statement>& statements, Catalog& catalog) {
// 语句级语义校验主流程：遍历整批语句，逐条做语义体检并更新临时目录。
    for (const auto& statement : statements) {
    // 按顺序处理每一条语句，顺序很重要（先建表才能插入）。
        if (statement.kind == "Begin" || statement.kind == "Commit" || statement.kind == "Rollback" ||
            statement.kind == "Savepoint" || statement.kind == "ReleaseSavepoint" || statement.kind == "RollbackTo" ||
            statement.kind == "Checkpoint") continue;
        // 事务控制与检查点语句不改目录结构，无需语义校验，直接跳过。
        if (statement.kind == "CreateIndex") { catalog.createIndex(statement); continue; }
        // 建索引走目录层登记，登记完继续下一条。
        if (statement.kind == "DropIndex") { catalog.dropIndex(statement); continue; }
        // 删索引同理。
        if (statement.kind == "Insert" && !statement.valueRows.empty()) {
        // 多行插入要拆成多条单行插入分别校验。
            auto single = statement;
            // 复制一份语句作为模板，避免改动原始语法树。
            single.valueRows.clear();
            // 去掉多行结构，改成单行形态。
            single.values.clear();
            // 同时清掉旧的单行值列表。
            for (const auto& row : statement.valueRows) {
            // 逐行处理。
                if (row.empty()) fail("INSERT row cannot be empty", statement.location);
                // 空行不合法。
                single.valueExpressions = row;
                // 把这一行装进模板。
                validate({single}, catalog);
                // 递归校验，复用单行插入那一套完整逻辑。
            }
            continue;
            // 多行插入已全部校验完毕，跳过后续单行逻辑。
        }
        if (statement.kind == "CreateTable") {
        // 建表语句。
            catalog.create(statement);
            // 交给目录层做全套校验并登记。
            continue;
            // 处理完继续下一条。
        }
        // X09 3.3: 派生表基座。内层 select 仍在真实 catalog 上校验；外层列绑定、
        // 类型与 WHERE 校验交由 planner 的 Scope 链完成（catalog 未知派生别名）。
        // X09 3.3: 派生表基座。内层 select 仍在真实 catalog 上校验；外层列绑定、
        // 类型与 WHERE 校验交由 planner 的 Scope 链完成（catalog 未知派生别名）。
        if (statement.fromSubquery != nullptr) {
        // 这条语句的 FROM 是一个派生表（子查询）。
            validate({*statement.fromSubquery}, catalog);
            // 先递归校验内层子查询本身；外层校验留给 planner。
            if (statement.kind == "Update" || statement.kind == "Delete") {
                const auto& source = *statement.fromSubquery;
                const bool wildcard = source.selectItems.size() == 1 && source.selectItems.front().expression &&
                    source.selectItems.front().expression->kind == "Wildcard" && source.selectItems.front().expression->value == "*";
                const bool updatable = wildcard && !source.fromSubquery && source.joins.empty() && !source.distinct &&
                    source.groupBy.empty() && !source.having && source.orderBy.empty() && !source.limit && source.offset == 0 && statement.joins.empty();
                if (!updatable) fail("Derived table is not updatable; use a single base-table SELECT * without JOIN, DISTINCT, grouping, ordering or pagination", statement.location);
                const auto* target = catalog.find(source.table);
                if (!target) fail("Table does not exist: " + source.table, statement.location);
                for (const auto& assignment : statement.assignments) (void)column(*target, assignment.column, statement.location);
            }
            continue;
            // 跳过常规校验流程。
        }
        auto binding = queryScope(statement, catalog);
        // 把主表与本次语句用到的 JOIN 合并成视图表，作为列解析的统一依据。
        const auto* table = &binding;
        // 用指针引用视图表，后面代码都按 table 访问。
        if (statement.kind == "Insert") {
        // 单行 INSERT 的校验。
            const auto names = insertColumns(statement, *table);
            // 算出目标列清单。
            const auto count = statement.valueExpressions.empty() ? statement.values.size() : statement.valueExpressions.size();
            // 算出值的个数，兼容"表达式形式"和"字面量文本形式"两种表示。
            if (names.size() != count) {
            // 列的个数与值的个数必须相等。
                fail("INSERT column/value count mismatch", statement.location);
                // 不等就报错。
            }
            std::unordered_set<std::string> seen;
            // 记录已处理过的列名，用来检测列清单内部的重复。
            std::vector<std::string> mismatches;
            // 收集所有类型不匹配的列，最后一次性报告出去。
            for (std::size_t i = 0; i < names.size(); ++i) {
            // 逐个处理"第 i 列 ← 第 i 个值"。
                const auto& name = names[i];
                // 取目标列名。
                const auto& target = column(*table, name, statement.location);
                // 解析列定义，列名非法会在这一步报错。
                if (!seen.insert(key(name)).second) fail("Duplicate INSERT column: " + name, statement.location);
                // 同一列在清单里出现两次属于非法。
                if (!statement.valueExpressions.empty() && !statement.valueExpressions[i]) fail("Missing INSERT expression", statement.location);
                // 表达式形式下第 i 个值缺失也要报错。
                const auto type = statement.valueExpressions.empty() ? literalType(statement.values[i], statement.location)
                    : statement.valueExpressions[i]->kind == "Default" ? literalType(target.defaultValue.value_or("NULL"), statement.location)
                    : expressionType(*statement.valueExpressions[i], Table{"", {}}, statement.location);
                // 推导这个值的类型：字面量形式直接判类型；DEFAULT 关键字取该列默认值的类型；表达式形式则按空表上下文求类型。
                if (statement.valueExpressions.empty())
                    rejectNarrowedInteger(statement.values[i], type, target.type, statement.location);
                else if (statement.valueExpressions[i]->kind == "Literal")
                    rejectNarrowedInteger(statement.valueExpressions[i]->value, type, target.type, statement.location);
                if (type == "null" && !target.nullable)
                // 往 NOT NULL 列插 NULL。
                    fail("NOT NULL constraint failed" + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, name)), statement.location);
                    // 报错并附上可能存在的约束名后缀，方便验收定位。
                if ((type == "null" && !target.nullable) || !assignable(type, key(target.type))) {
                // 再次判断是否不可赋值（这里主要是为了收集信息，第一次已单独报过 NOT NULL）。
                    // 收集全部不匹配的列，一次报告（验收示例要求同时给出 id 与 name 两处）。
                    // 收集全部不匹配的列，一次报告（验收示例要求同时给出 id 与 name 两处）。
                    mismatches.push_back(statement.table + "." + name + " expects " + typeLabel(target.type) +
                        ", but " + typeLabel(type) + " found");
                    // 记下"某列期望某类型、实际给了某类型"这句说明。
                }
            }
            if (!mismatches.empty()) {
            // 有收集到的不匹配项。
                std::string message = "INSERT type mismatch: ";
                // 拼一个统一的错误前缀。
                for (std::size_t i = 0; i < mismatches.size(); ++i) message += (i == 0 ? "" : "; ") + mismatches[i];
                // 用分号把各列的问题串起来。
                fail(message, statement.location);
                // 一次性抛出，让用户在同一轮就看到所有类型问题。
            }
            for (const auto& target : table->columns)
            // 再检查有没有该给值却没给的列。
                if (!seen.contains(key(target.name)) && !target.nullable && !target.defaultValue)
                // 既不在列清单里，又不允许为空，又没有默认值，属于缺值。
                    fail("Missing required INSERT column: " + target.name + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, target.name)), statement.location);
                    // 逐列报错（保持与既有验收输出一致）。
        }
        if (statement.kind == "Update") {
        // UPDATE 语句的校验。
            std::unordered_set<std::string> seen;
            // 记录已出现的赋值列，检测重复赋值。
            if (statement.assignments.empty()) fail("UPDATE requires assignments", statement.location);
            // UPDATE 必须有至少一个赋值项。
            for (const auto& item : statement.assignments) {
            // 逐个校验赋值项。
                const auto& target = column(*table, item.column, statement.location);
                // 解析被赋值列。
                if (!seen.insert(key(item.column)).second) fail("Duplicate UPDATE column: " + item.column, statement.location);
                // 同一列被赋值两次属于非法。
                if (!item.expression) fail("Missing UPDATE expression", statement.location);
                // 赋值表达式不能为空。
                const auto type = item.expression->kind == "Default" ? literalType(target.defaultValue.value_or("NULL"), statement.location)
                    : expressionType(*item.expression, *table, statement.location);
                // 推导赋值来源类型：DEFAULT 取该列默认值类型，否则在视图表上下文里求表达式类型。
                if (item.expression->kind == "Literal")
                    rejectNarrowedInteger(item.expression->value, type, target.type, statement.location);
                if (type == "null" && !target.nullable)
                // 往 NOT NULL 列赋 NULL。
                    fail("NOT NULL constraint failed" + notNullConstraintSuffix(*catalog.find(statement.table), resolveColumnIndex(*table, item.column)), statement.location);
                    // 报错并带上约束名后缀。
                if ((type == "null" && !target.nullable) || !assignable(type, key(target.type)))
                // 或者类型本身无法赋给该列。
                    fail("UPDATE value type mismatch for column: " + item.column, statement.location);
                    // 按列报出类型不匹配。
            }
        }
        if (statement.selectItems.empty()) for (const auto& name : statement.selectList) {
        // 语句没有结构化投影列表时（例如 SELECT 列表仍以字符串形式保存），逐个校验这些列名。
            if (resolveColumnName(*table, name, statement.location) != "*") column(*table, name, statement.location);
            // 通配符 * 放行；否则按真实列解析，列不存在会在此报错。
        }
        if (statement.kind == "Select") (void)analyzeSelect(statement, *table);
        // SELECT 走前面那套聚合/投影/排序/分组联合体检。
        if (statement.where) {
        // 有 WHERE 条件时校验它。
            const auto type = expressionType(*statement.where, *table, statement.location);
            // 在视图表上下文推导条件类型。
            if (type != "bool" && type != "null") fail("WHERE requires a BOOL expression", statement.location);
            // WHERE 必须是布尔表达式。
        }
    }
}

Catalog compileSnapshot(const std::vector<sql::Statement>& statements, const Catalog& catalog) {
// 在不改动真实目录的前提下预演一遍语句，返回"执行完这批语句后"的目录快照。
    Catalog snapshot = catalog;
    // 拷贝一份目录，预演只作用在副本上。
    validate(statements, snapshot);
    // 在副本上跑全部语义校验（建表等语句会真的改这份副本）。
    return snapshot;
    // 返回预演后的目录，planner 据此获得准确的表结构。
}
std::shared_ptr<sql::Expr> resolveOrder(const sql::Statement& statement, const sql::OrderItem& item, const Table& table) {
// 处理 ORDER BY 里"写的是投影别名"的情况，把它换成真正的投影表达式。
    if (!item.expression) fail("Missing ORDER BY expression", statement.location);
    // 排序项不能没有表达式。
    if (item.expression->kind != "Identifier") return item.expression;
    // 只有裸标识符才可能是别名；其他形态（如 1+1）直接原样返回。
    std::shared_ptr<sql::Expr> match;
    // 记录匹配到的投影表达式。
    for (const auto& projection : statement.selectItems) {
    // 在投影列表里找同名别名。
        if (!projection.alias.empty() && key(projection.alias) == key(item.expression->value)) {
        // 别名非空且与 ORDER BY 里的标识符同名。
            if (match) fail("Ambiguous ORDER BY alias", item.expression->location);
            // 同名别名出现两次，无法判断排的是哪一个，报歧义。
            match = projection.expression;
            // 记录命中的投影表达式。
        }
    }
    if (match) {
    // 确实找到了别名对应的投影。
        for (std::size_t i = 0; i < table.columns.size(); ++i) {
        // 再检查这个别名是否与源表列名撞车。
            const auto& source = table.columns[i];
            // 取当前源列。
            if (key(source.name) == key(item.expression->value) &&
                !(match->kind == "Identifier" && resolveColumnIndex(table, match->value, match->location) == i))
            // 条件含义：名字与标识符相同的源列存在，而且别名指向的并不是这一列本身。
                fail("ORDER BY alias conflicts with source column", item.expression->location);
                // 这种同名会让语义产生歧义，直接拒绝。
        }
        return match;
        // 校验通过，把 ORDER BY 换成别名指向的投影表达式。
    }
    return item.expression;
    // 没有同名别名，说明写的就是普通列引用或表达式，原样返回。
}
}
