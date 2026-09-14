#include "minisql/optimizer/optimizer.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/decimal.hpp"
#include <set>

namespace minisql::optimizer {
namespace {
using json = nlohmann::json;
// 简写 JSON 命名空间。
bool literal(const json& expr) { return expr.is_object() && expr.value("kind", "") == "Literal"; }
// 判断一个表达式节点是不是字面量：是对象，且 kind 为 Literal。
bool boolean(const json& expr) { return literal(expr) && expr.contains("value") && expr.at("value").is_boolean(); }
// 判断是不是"布尔字面量"：先得是字面量，还要带 value 字段且该字段确实是布尔值。
ExactDecimal decimalConstant(const json& expression) {
// 把一个定点数字面量节点还原成精确十进制值。
    const auto type = decimalType(expression.at("type").get<std::string>());
    // 解析节点上标注的类型，判断它是不是 decimal(p,s)。
    return type ? ExactDecimal::parse(expression.at("value").get<std::string>(), type->precision, type->scale) : ExactDecimal::fromInteger(expression.at("value").get<std::int64_t>());
    // 是定点数就按它自己的精度标度解析文本；否则按 64 位整数取值。
    // 两条分支最终都得到同一个 ExactDecimal 类型，后面可以统一做比较与运算。
}
json constant(const json& original, json value) {
// 造一个常量节点；original 用来继承源位置，value 是常量值。
    return {{"kind", "Literal"}, {"type", "bool"}, {"value", value},
            // 种类固定为字面量，类型先按布尔写（调用方通常会再改），值就是传入的值。
            {"line", original.value("line", 0)}, {"column", original.value("column", 0)}};
            // 行号列号从原节点继承，这样改写后的常量仍然能指向原来的位置。
}
void record(json& changes, const char* rule, std::size_t statement, const json& before, const json& after) {
// 记录一次改写：哪条规则、第几条语句、改写前是什么、改写后是什么。
    changes.push_back({{"ruleId", rule}, {"statementIndex", statement}, {"before", before}, {"after", after}});
    // 追加一条改写记录，供 EXPLAIN、优化过程展示与测试断言使用。
}
json rewrite(json expression, const Options& options, json& changes, std::size_t statement, std::size_t depth = 0) {
// 对表达式树做规则改写，返回改写后的表达式；按值接收以便就地修改副本。
    if (depth > 256) throw MiniSqlError(ErrorCode::Internal, "Optimizer expression depth exceeded");
    // 深度保护：表达式过分嵌套时停止改写，避免递归栈溢出。
    const auto kind = expression.value("kind", "");
    // 取出节点种类。
    if (kind == "Literal" || kind == "Identifier" || kind == "Exists" || kind == "ScalarSubquery") return expression;
    // 叶子节点与子查询节点直接返回：它们自身没有可折叠的运算，内部逻辑也不在这层改写。
    if (kind == "InSubquery") {
    // IN 子查询：只有左操作数是普通表达式，需要往下改写。
        expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
        // 递归改写左子树。
        return expression;
        // 返回改写后的节点。
    }
    if (kind == "Cast") {
    // 类型转换节点：被转换的表达式需要改写。
        expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
        // 递归改写左子树。
        return expression;
        // 返回改写后的节点。
    }
    if (kind != "Unary" && kind != "Binary") throw MiniSqlError(ErrorCode::Internal, "Unsupported optimizer expression");
    // 到这里只可能是单目或双目运算；其它种类说明表达式结构不符合预期，按内部错误拒绝。
    expression["left"] = rewrite(expression.at("left"), options, changes, statement, depth + 1);
    // 先改写左子树。
    if (kind == "Binary") expression["right"] = rewrite(expression.at("right"), options, changes, statement, depth + 1);
    // 双目运算还要改写右子树。
    const auto op = expression.at("operator").get<std::string>();
    // 取出运算符。
    const auto& left = expression.at("left");
    // 引用改写后的左子树，后面的规则都要看它。
    auto replace = [&](json after, const char* rule) {
    // 局部工具：用新节点替换当前节点，并做类型一致性检查与记录。
        if (after.value("type", "") == "null" && expression.at("type") == "bool") after["type"] = "bool";
        // 若新常量暂时没标类型（null），而原节点是布尔表达式，就补成 bool，
        // 避免因为"常量类型未知"误判为类型变化。
        if (expression.at("type") != after.at("type"))
        // 改写前后类型必须完全一致。
            throw MiniSqlError(ErrorCode::Internal, "Optimizer rewrite changed expression type");
            // 类型变了说明规则写错了（会让上层计划类型推导失效），必须立刻暴露。
        record(changes, rule, statement, expression, after);
        // 记录这次改写。
        return after;
        // 返回新节点。
    };
    // replace 定义结束。
    if (options.constantArithmetic && isArithmetic(op) && decimalType(expression.at("type").get<std::string>()) &&
        // 规则一：定点数常量算术折叠。要求开关打开、是算术运算、结果是定点数、
        literal(left) && !left.at("value").is_null() && (kind == "Unary" || (literal(expression.at("right")) && !expression.at("right").at("value").is_null()))) {
        // 右操作数（双目时）也必须是常量，且两侧都不能是 NULL。
        try {
        // 折叠运算可能会因为精度或除零失败，所以包在 try 里。
            const auto a = decimalConstant(left);
            // 左操作数转成精确十进制。
            const auto value = kind == "Unary" ? (op == "-" ? a.negated() : a) : a.arithmetic(op, decimalConstant(expression.at("right")));
            // 单目时只有取负有意义（其它单目算子在前面已被排除）；双目时按运算符做精确运算。
            auto folded = expression;
            // 复制原节点，准备改成常量节点。
            folded["kind"] = "Literal";folded["value"] = value.format();
            // 种类改成字面量，值改写成运算结果的规范文本。
            folded.erase("left");folded.erase("right");folded.erase("operator");
            // 删掉已经没有意义的子树与运算符字段，保持节点结构干净。
            return replace(folded, "constant-arithmetic");
            // 用常量替换原节点并返回。
        } catch (const MiniSqlError& error) {
        // 运算失败（如除零、精度溢出）。
            if (error.code() != ErrorCode::Execution) throw;
            // 只有执行类错误才是"这个常量折叠做不了"；其它错误属于程序缺陷，继续向上抛。
        }
        // 失败时不做任何改写，保留原表达式交运行时处理，保证错误时机不变。
    }
    if (options.constantArithmetic && isArithmetic(op) && literal(left) && left.at("value").is_number_integer() &&
        // 规则二的入口条件：整数常量参与算术运算，且左操作数是整数。
        (kind == "Unary" || (literal(expression.at("right")) && expression.at("right").at("value").is_number_integer()))) {
        // 双目时右操作数也必须是整数常量。
        try {
        // 整数运算可能溢出，用异常区分"折叠不了"。
            auto a = left.at("value").get<std::int64_t>();
            // 取出左操作数的 64 位整数形式。
            auto b = kind == "Unary" ? a : expression.at("right").at("value").get<std::int64_t>();
            // 单目时先把 b 记成 a 占位；双目时取右操作数。
            if (kind == "Unary") a = 0;
            // 单目运算按 "0 op b" 拆成双目形式，这样下面可以统一调用同一个运算函数。
            auto folded = constant(expression, false);
            // 先造一个占位常量节点，稍后填入真实值与类型。
            folded["type"] = expression.at("type");
            // 类型沿用原节点，保证 replace 的类型一致性检查能通过。
            folded["value"] = expression.at("type") == "bigint" ? json(arithmetic64(op, a, b)) : json(arithmetic(op, static_cast<std::int32_t>(a), static_cast<std::int32_t>(b)));
            // bigint 走 64 位运算接口，其余按 32 位运算并做溢出检查后落回。
            return replace(folded, "constant-arithmetic");
            // 用常量替换原节点。
        } catch (const MiniSqlError& error) {
        // 运算失败（典型是溢出）。
            if (error.code() != ErrorCode::Execution) throw;
            // 只有执行类错误才当作"折叠不了"，其它错误继续上抛。
            // 保留可能抛错的子树，运行时由短路规则决定是否求值。
            // 不改写的原因：这个子表达式本来会在运行时可能抛错，
            // 折叠成常量会把这个错误提前到优化期，从而改变错误的触发时机与是否触发。
            // 保留可能抛错的子树，运行时由短路规则决定是否求值。
        }
    }
    if (options.booleanSimplification && literal(left)) {
    // 规则三：布尔化简中"看左操作数"的那部分。
        const auto& value = left.at("value");
        // 取出左操作数的值。
        if (op == "IS NULL" || op == "IS NOT NULL")
        // IS NULL / IS NOT NULL 作用在常量上可以直接算出真假。
            return replace(constant(expression, op == "IS NULL" ? value.is_null() : !value.is_null()), "boolean-simplification");
            // 注意值本身可能为 NULL，此时 IS NULL 为真、IS NOT NULL 为假。
        if (op == "NOT" && (value.is_boolean() || value.is_null()))
        // NOT 作用在布尔或 NULL 常量上。
            return replace(constant(expression, value.is_null() ? json(nullptr) : json(!value.get<bool>())), "boolean-simplification");
            // 三值逻辑下 NOT NULL 仍然是 NULL，所以 NULL 要保持 NULL。
    }
    if (kind != "Binary") return expression;
    // 到这里剩下的规则都只针对双目运算；单目到此为止。
    const auto& right = expression.at("right");
    // 引用右子树。
    if (options.booleanSimplification && (op == "AND" || op == "OR")) {
    // AND / OR 的化简。
        // 仅同时为常量时使用三值真值表，避免 NULL 吸收规则跳过右侧异常。
        // 解释：TRUE OR x 在数学上恒真，但若 x 会抛错，直接折叠成 TRUE 就把错误吞掉了，
        // 所以只有两侧都是常量时才敢按真值表算。
        // 仅同时为常量时使用三值真值表，避免 NULL 吸收规则跳过右侧异常。
        if (literal(left) && literal(right)) {
        // 两侧都是常量。
            const auto& a = left.at("value");
            // 左值。
            const auto& b = right.at("value");
            // 右值。
            if ((a.is_boolean() || a.is_null()) && (b.is_boolean() || b.is_null())) {
            // 两侧都必须落在 {TRUE, FALSE, NULL} 里。
                const bool absorbing = op == "OR";
                // OR 的吸收元素是 TRUE，AND 的吸收元素是 FALSE；这里用个布尔把两种情况合并。
                const json value = a == absorbing || b == absorbing ? json(absorbing) :
                    // 任一侧等于吸收元素，整体就是吸收元素。
                    a.is_null() || b.is_null() ? json(nullptr) : json(!absorbing);
                    // 否则只要还有 NULL，结果就是 NULL（三值逻辑）；都不是就是另一个元素。
                return replace(constant(expression, value), "boolean-simplification");
                // 用算出的常量替换。
            }
        }
        if (boolean(left)) {
        // 左操作数是布尔常量。
            const bool value = left.at("value").get<bool>();
            // 取出它的真假。
            if ((op == "AND" && !value) || (op == "OR" && value))
            // FALSE AND x 恒假、TRUE OR x 恒真。
                return replace(constant(expression, value), "boolean-simplification");
                // 直接用左值替换整个表达式。
            return replace(right, "boolean-simplification");
            // 其余情况（TRUE AND x、FALSE OR x）结果就等于右操作数，直接换成右子树。
        }
        // 只消去右侧恒等值，保留左侧求值，避免以后加入算术错误时被错误隐藏。
        // 解释：这里只处理"右操作数是恒等元素"的情况，因为左侧必须先求值，
        // 直接消掉右侧不会改变左侧的求值顺序与可能产生的错误。
        // 只消去右侧恒等值，保留左侧求值，避免以后加入算术错误时被错误隐藏。
        if (boolean(right) && ((op == "AND" && right.at("value").get<bool>()) ||
                              // x AND TRUE 等价于 x，
                              (op == "OR" && !right.at("value").get<bool>())))
                              // x OR FALSE 也等价于 x。
            return replace(left, "boolean-simplification");
            // 直接用左子树替换整个表达式。
    }
    if (options.constantComparison && literal(left) && literal(right)) {
    // 规则四：常量比较折叠，两侧都是字面量。
        const auto& a = left.at("value");
        // 左值。
        const auto& b = right.at("value");
        // 右值。
        if (a.is_null() || b.is_null()) {
        // 任一操作数为 NULL 时，六种比较的结果都是 UNKNOWN。
            if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=")
            // 只对六种比较运算符成立。
                return replace(constant(expression, nullptr), "constant-comparison");
                // 折叠成 NULL 常量。
            return expression;
            // 其它运算符（如 IS NULL）不走这条分支。
        }
        bool value;
        // 存放比较结果。
        if (decimalType(left.at("type").get<std::string>()) || decimalType(right.at("type").get<std::string>())) {
        // 任一侧是定点数时，必须用精确十进制比较，不能退化成 double。
            const auto order = decimalConstant(left).compare(decimalConstant(right));
            // 先算出大小关系（-1/0/1）。
            if (op == "=") value = order == 0;
            // 相等。
            else if (op == "!=") value = order != 0;
            else if (op == "<") value = order < 0;
            else if (op == "<=") value = order <= 0;
            else if (op == ">") value = order > 0;
            else if (op == ">=") value = order >= 0;
            else return expression;
            // 不是比较运算符就放弃折叠。
        }
        else if (op == "=") value = a == b;
        // 非定点数按 JSON 原生比较：
        else if (op == "!=") value = a != b;
        else if (op == "<") value = a < b;
        else if (op == "<=") value = a <= b;
        else if (op == ">") value = a > b;
        else if (op == ">=") value = a >= b;
        else return expression;
        // 其它运算符放弃折叠。
        return replace(constant(expression, value), "constant-comparison");
        // 用布尔常量替换这次比较。
    }
    return expression;
    // 没有规则命中，原样返回。
}
bool safePushdownExpression(const json& expression, std::size_t depth = 0) {
// 判断一个谓词能不能安全下推到连接的一侧。
// "安全"的含义：表达式只由列引用、字面量和不会出错的比较/布尔运算组成，
// 因此下推不会改变结果，也不会改变可能出现的错误时机。
    if (!expression.is_object() || depth > 64) return false;
    // 不是对象，或者嵌套过深，一律认为不安全。
    const auto kind = expression.value("kind", "");
    // 取出节点种类。
    if (kind == "Literal" || kind == "Identifier") return true;
    // 字面量与列引用自身不会出错，安全。
    if (kind == "Unary") {
    // 单目运算。
        const auto op = expression.value("operator", "");
        // 取出运算符。
        if (op != "NOT" && op != "IS NULL" && op != "IS NOT NULL") return false;
        // 只有这三种单目运算被允许；取负之类可能溢出，故排除。
        return expression.contains("left") && safePushdownExpression(expression.at("left"), depth + 1);
        // 还要保证操作数存在，并且递归检查它。
    }
    if (kind != "Binary") return false;
    // 其余种类（如子查询、聚合）一律不安全。
    const auto op = expression.value("operator", "");
    // 取出运算符。
    if (op != "AND" && op != "OR" && op != "=" && op != "!=" && op != "<" && op != "<=" && op != ">" && op != ">=") return false;
    // 只允许逻辑运算与六种比较运算；算术运算可能溢出，故排除。
    return expression.contains("left") && expression.contains("right") &&
        // 两个操作数都必须存在，
        safePushdownExpression(expression.at("left"), depth + 1) && safePushdownExpression(expression.at("right"), depth + 1);
        // 并且左右子树都要递归通过检查。
}
// X25：表达式引用到的稳定列身份（sql::ColumnId 原始值）。与 collectColumns
// 的区别是它不随槽位平移而变化，因此裁剪与下推可以直接比对。
void collectBindings(const json& expression, std::set<std::uint32_t>& bindings, std::size_t depth = 0) {
    if (!expression.is_object() || depth > 64) return;
    if (expression.value("kind", "") == "Identifier") {
        const auto binding = expression.value("binding", std::uint32_t{0});
        if (binding) bindings.insert(binding);
    }
    if (expression.contains("left")) collectBindings(expression.at("left"), bindings, depth + 1);
    if (expression.contains("right")) collectBindings(expression.at("right"), bindings, depth + 1);
}

// 表达式引用到的关系身份。空集表示「没有任何带身份的列引用」，
// 调用方据此退回旧的槽位判断。
void collectRelations(const json& expression, std::set<std::uint32_t>& relations, std::size_t depth = 0) {
    if (!expression.is_object() || depth > 64) return;
    if (expression.value("kind", "") == "Identifier") {
        const auto relation = expression.value("relation", std::uint32_t{0});
        if (relation) relations.insert(relation);
    }
    if (expression.contains("left")) collectRelations(expression.at("left"), relations, depth + 1);
    if (expression.contains("right")) collectRelations(expression.at("right"), relations, depth + 1);
}

std::set<std::uint32_t> planRelations(const sql::LogicalPlan& plan) {
    std::set<std::uint32_t> relations;
    for (const auto& column : plan.output)
        if (column.relation) relations.insert(column.relation);
    return relations;
}

void collectColumns(const json& expression, std::set<std::size_t>& columns, std::size_t depth = 0) {
// 收集一个表达式里引用到的全部列编号，用于判断谓词属于连接的哪一侧。
    if (!expression.is_object() || depth > 64) return;
    // 不是对象或过深就停止，避免栈溢出。
    if (expression.value("kind", "") == "Identifier" && expression.contains("columnId"))
    // 命中一个列引用节点，且它带列编号。
        columns.insert(expression.at("columnId").get<std::size_t>());
        // 把编号记进集合（集合天然去重）。
    if (expression.contains("left")) collectColumns(expression.at("left"), columns, depth + 1);
    // 递归左子树。
    if (expression.contains("right")) collectColumns(expression.at("right"), columns, depth + 1);
    // 递归右子树。
}
json shiftColumns(json expression, std::size_t offset, std::size_t depth = 0) {
// 把表达式里的列编号整体减去 offset。
// 用途：下推到连接右侧的谓词，其列编号是按"连接输出"编的，必须换算成右侧子算子的编号。
    if (!expression.is_object() || depth > 64) return expression;
    // 不是对象或过深就原样返回。
    if (expression.value("kind", "") == "Identifier" && expression.contains("columnId")) {
    // 命中的一个列引用。
        const auto id = expression.at("columnId").get<std::size_t>();
        // 取出它在连接输出里的编号。
        if (id < offset) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown column shift underflow");
        // 编号比偏移还小，说明这个谓词根本不属于右侧，属于内部逻辑错误。
        expression["columnId"] = id - offset;
        // 换算成右侧子算子内的编号。
    }
    if (expression.contains("left")) expression["left"] = shiftColumns(expression.at("left"), offset, depth + 1);
    // 递归处理左子树。
    if (expression.contains("right")) expression["right"] = shiftColumns(expression.at("right"), offset, depth + 1);
    // 递归处理右子树。
    return expression;
    // 返回换算后的表达式。
}
void splitConjuncts(json expression, std::vector<json>& terms, std::size_t depth = 0) {
// 把一个 AND 表达式拆成若干个合取项，方便逐项判断能不能下推。
    if (depth > 64) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown depth exceeded");
    // 深度保护：过深的 AND 链直接按内部错误拒绝。
    if (expression.is_object() && expression.value("kind", "") == "Binary" && expression.value("operator", "") == "AND") {
    // 当前节点是 AND。
        splitConjuncts(expression.at("left"), terms, depth + 1);
        // 递归拆左子树。
        splitConjuncts(expression.at("right"), terms, depth + 1);
        // 递归拆右子树。
    } else terms.push_back(std::move(expression));
    // 不是 AND 就是最小合取项，直接收进结果。
}
json combineConjuncts(std::vector<json> terms) {
// 把若干合取项重新用 AND 串起来，是 splitConjuncts 的逆操作。
    if (terms.empty()) throw MiniSqlError(ErrorCode::Internal, "Predicate pushdown produced empty predicate");
    // 空集合无法表示成合法谓词，属于内部错误。
    auto result = std::move(terms.front());
    // 用第一项作为起点。
    for (std::size_t i = 1; i < terms.size(); ++i) {
    // 依次把后面的项 AND 上去。
        const auto line = result.value("line", 0);
        // 记住当前结果的位置行号。
        const auto column = result.value("column", 0);
        // 以及列号。
        result = {{"kind", "Binary"}, {"operator", "AND"}, {"type", "bool"}, {"nullable", true},
            // 新节点是布尔类型的 AND 运算，
            {"left", std::move(result)}, {"right", std::move(terms[i])},
            // 左边是已拼接结果，右边是新项，
            {"line", line}, {"column", column}};
            // 位置信息沿用原来的，保证报错仍指向用户写的谓词。
    }
    // 拼接结束。
    return result;
    // 返回合取结果。
}
bool columnId(const json& expression, std::size_t& id) {
// 判断节点是不是"带列编号的列引用"，是就把编号写进 id。
    if (!expression.is_object() || expression.value("kind", "") != "Identifier" || !expression.contains("columnId")) return false;
    // 三条同时满足才算：是对象、种类是列引用、带 columnId 字段。
    id = expression.at("columnId").get<std::size_t>();
    // 输出列编号。
    return true;
    // 返回成功。
}
bool hashJoinKeys(const json& predicate, std::size_t leftSize, std::size_t& leftKey, std::size_t& rightKey) {
// 判断连接谓词是不是"左表的某一列 = 右表的某一列"，是就给出两侧的键。
    if (!predicate.is_object() || predicate.value("kind", "") != "Binary" || predicate.value("operator", "") != "=") return false;
    // 必须是等值比较。
    std::size_t a{}, b{};
    // 两侧的列编号。
    if (!columnId(predicate.at("left"), a) || !columnId(predicate.at("right"), b)) return false;
    // 两侧都必须是裸列引用；表达式参与等值比较不能直接用哈希连接。
    if (a < leftSize && b >= leftSize) { leftKey = a; rightKey = b - leftSize; return true; }
    // 左小右大：左侧编号直接可用，右侧编号要减去左表列数换算。
    if (b < leftSize && a >= leftSize) { leftKey = b; rightKey = a - leftSize; return true; }
    // 反过来同理，这样等号两边写反也能识别。
    return false;
    // 两侧都属于同一张表，说明不是跨表等值条件，不能做哈希连接。
}
bool pruneProjectColumns(sql::LogicalPlan& project, json& changes, std::size_t statement) {
// 列裁剪：把 Project 下方扫描算子的输出列，缩减到上层真正用到的那几列。
    if (project.kind != "Project" || project.children.size() != 1 || project.projections.empty()) return false;
    // 只处理"投影 + 恰好一个孩子 + 有投影表达式"这种规整形态。
    auto& child = project.children.front();
    // 取出那个孩子。
    sql::LogicalPlan* filter = nullptr;
    // 孩子是不是一个过滤算子；不是则为空。
    sql::LogicalPlan* scan = nullptr;
    // 扫描算子，一定存在。
    if (child.kind == "SeqScan") scan = &child;
    // 形态一：Project 直接架在扫描上。
    else if (child.kind == "Filter" && child.children.size() == 1 && child.children.front().kind == "SeqScan") {
    // 形态二：Project → Filter → SeqScan。
        filter = &child;
        // 记录过滤器。
        scan = &child.children.front();
        // 记录扫描算子。
    } else return false;
    // 其它形态（连接、聚合等）不在本规则范围内。
    std::set<std::size_t> required;
    // 收集真正被用到的列编号。
    for (const auto& expression : project.projections) collectColumns(expression, required);
    // 投影表达式里引用到的列必须保留。
    if (filter) collectColumns(filter->predicate, required);
    // 过滤条件里引用到的列也必须保留，否则过滤就没法算了。
    std::set<std::uint32_t> requiredBindings;
    for (const auto& expression : project.projections) collectBindings(expression, requiredBindings);
    if (filter) collectBindings(filter->predicate, requiredBindings);
    // 有稳定身份就按身份裁剪：身份不随裁剪或下推平移，因此不需要任何重基。
    // 没有身份（旧计划文档、表达式列）时退回槽位比较，行为与改造前一致。
    const auto keep = [&](const sql::PlanColumn& column) {
        if (column.binding && !requiredBindings.empty()) return requiredBindings.contains(column.binding);
        return required.contains(column.columnId);
    };
    std::vector<sql::PlanColumn> pruned;
    // 裁剪后的列清单。
    for (const auto& column : scan->output) if (keep(column)) pruned.push_back(column);
    if (pruned.size() == scan->output.size()) return false;
    // 一列都没裁掉，说明这条规则没有产生效果，按"未改写"返回。
    const auto before = sql::serializePlans({project});
    // 记下改写前的计划形态，用于生成改写记录。
    scan->output = std::move(pruned);
    // 就地替换扫描算子的输出列清单。
    record(changes, "prune-columns", statement, before, sql::serializePlans({project}));
    // 记录这次裁剪。
    return true;
}
void collectPlanBindings(const sql::LogicalPlan& plan, std::set<std::uint32_t>& bindings,
                         std::set<std::size_t>& slots) {
    const auto collect = [&](const json& expression) {
        collectBindings(expression, bindings);
        collectColumns(expression, slots);
    };
    collect(plan.predicate);
    for (const auto& value : plan.projections) collect(value);
    for (const auto& value : plan.sortKeys) collect(value);
    for (const auto& value : plan.groupKeys) collect(value);
    for (const auto& value : plan.insertExpressions) collect(value);
    for (const auto& row : plan.insertRows)
        if (row.is_object() && row.contains("expressions")) for (const auto& value : row.at("expressions")) collect(value);
    for (const auto& aggregate : plan.aggregates)
        if (aggregate.is_object() && aggregate.contains("argument") && !aggregate.at("argument").is_null()) collect(aggregate.at("argument"));
}
bool pruneRequiredColumns(sql::LogicalPlan& root, json& changes, std::size_t statement) {
    const auto before = sql::serializePlans({root});
    bool changed = false;
    std::function<void(sql::LogicalPlan&, std::set<std::uint32_t>, std::set<std::size_t>, bool)> visit;
    visit = [&](sql::LogicalPlan& plan, std::set<std::uint32_t> requiredBindings,
                std::set<std::size_t> requiredSlots, bool preservePhysicalRow) {
        collectPlanBindings(plan, requiredBindings, requiredSlots);
        const bool join = plan.kind == "NestedLoopJoin" || plan.kind == "HashJoin" || plan.kind == "LeftJoin" ||
                          plan.kind == "RightJoin" || plan.kind == "FullJoin";
        const bool dml = plan.kind == "Update" || plan.kind == "Delete" || plan.kind == "Insert";
        if (plan.kind == "SeqScan" || plan.kind == "IndexScan") {
            if (preservePhysicalRow || dml) return;
            const bool identities = std::any_of(plan.output.begin(), plan.output.end(), [](const auto& column) { return column.binding != 0; });
            std::vector<sql::PlanColumn> output;
            for (const auto& column : plan.output) {
                const bool keep = identities ? requiredBindings.contains(column.binding) : requiredSlots.contains(column.columnId);
                if (keep) output.push_back(column);
            }
            if (output.size() != plan.output.size()) { plan.output = std::move(output); changed = true; }
            return;
        }
        for (auto& child : plan.children)
            visit(child, requiredBindings, requiredSlots, preservePhysicalRow || join || dml);
    };
    visit(root, {}, {}, false);
    if (changed) record(changes, "prune-columns", statement, before, sql::serializePlans({root}));
    return changed;
}

std::string folded(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}
double estimateRows(const sql::LogicalPlan& plan, const Options& options) {
    if (plan.kind == "SeqScan" || plan.kind == "IndexScan") {
        const auto found = options.tableRows.find(folded(plan.table));
        const auto rows = found == options.tableRows.end() ? options.defaultTableRows : found->second;
        return plan.kind == "IndexScan" ? std::max(1.0, rows * 0.1) : rows;
    }
    if (plan.children.empty()) return 1.0;
    const auto left = estimateRows(plan.children.front(), options);
    if (plan.kind == "Filter") return std::max(0.0, left * 0.33);
    if (plan.kind == "Limit" && plan.limit) return std::min(left, static_cast<double>(*plan.limit));
    if (plan.kind == "Aggregate") return plan.groupKeys.empty() ? 1.0 : std::max(1.0, std::sqrt(left));
    if ((plan.kind == "NestedLoopJoin" || plan.kind == "HashJoin") && plan.children.size() == 2) {
        const auto right = estimateRows(plan.children[1], options);
        std::size_t leftKey{}, rightKey{};
        if (!hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey)) return left * right * 0.33;
        const auto leftDistinct = leftKey < plan.children[0].output.size() &&
            (plan.children[0].output[leftKey].unique || plan.children[0].output[leftKey].primaryKey) ? left : std::max(1.0, std::sqrt(left));
        const auto rightDistinct = rightKey < plan.children[1].output.size() &&
            (plan.children[1].output[rightKey].unique || plan.children[1].output[rightKey].primaryKey) ? right : std::max(1.0, std::sqrt(right));
        return left * right / std::max(leftDistinct, rightDistinct);
    }
    return left;
}
bool chooseJoinAlgorithm(sql::LogicalPlan& plan, const Options& options, json& changes, std::size_t statement) {
    if ((plan.kind != "NestedLoopJoin" && plan.kind != "HashJoin") || plan.children.size() != 2) return false;
    const auto leftRows = estimateRows(plan.children[0], options);
    const auto rightRows = estimateRows(plan.children[1], options);
    const auto nestedCost = leftRows * rightRows;
    std::size_t leftKey{}, rightKey{};
    const bool hashable = hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey);
    const auto rowBytes = std::max<std::size_t>(32, plan.children[1].output.size() * 16);
    const auto buildBytes = rightRows * static_cast<double>(rowBytes);
    const auto spillPasses = options.memoryBudgetBytes == 0 ? 1.0 : std::max(0.0, std::ceil(buildBytes / options.memoryBudgetBytes) - 1.0);
    const auto hashCost = hashable ? leftRows + rightRows + spillPasses * rightRows : std::numeric_limits<double>::infinity();
    const auto selected = hashable && hashCost <= nestedCost ? "HashJoin" : "NestedLoopJoin";
    json decision = {{"model", "join-cost-v1"}, {"estimatedRows", {{"left", leftRows}, {"right", rightRows}, {"output", estimateRows(plan, options)}}},
        {"distinctEstimate", "unique-key-or-sqrt-rows"}, {"memoryBudgetBytes", options.memoryBudgetBytes}, {"estimatedBuildBytes", buildBytes},
        {"candidates", json::array({{{"kind", "NestedLoopJoin"}, {"estimatedCost", nestedCost}},
                                      {{"kind", "HashJoin"}, {"estimatedCost", hashable ? json(hashCost) : json(nullptr)}, {"eligible", hashable}}})},
        {"selected", selected}, {"reason", hashable ? "minimum-estimated-cost" : "no-direct-equality-key"}};
    const bool changed = plan.kind != selected || plan.optimizerDecision != decision;
    if (!changed) return false;
    const auto beforeKind = plan.kind;
    plan.kind = selected;
    plan.optimizerDecision = decision;
    record(changes, "hash-join", statement, {{"kind", beforeKind}}, {{"kind", selected}, {"decision", decision}});
    return true;
    // 返回"已改写"。
}
bool pushPredicateIntoJoin(sql::LogicalPlan& filter, json& changes, std::size_t statement) {
// 谓词下推：把 Filter 上属于某一侧的合取项，下推到连接那一侧的输入里。
    if (filter.kind != "Filter" || filter.children.size() != 1 || (filter.children.front().kind != "NestedLoopJoin" && filter.children.front().kind != "HashJoin")) return false;
    // 只处理"过滤 + 恰好一个连接孩子"这种形态，且连接必须是内连接类算子。
    auto joined = std::move(filter.children.front());
    // 把连接算子从孩子位置暂时取出来（避免接下来写孩子时把它自己覆盖掉）。
    auto& join = joined;
    // 给取出的连接算子起个短名字。
    if (join.children.size() != 2 || !safePushdownExpression(filter.predicate)) {
    // 连接必须恰好两个输入，且整个谓词都要通过"可安全下推"检查。
        filter.children.front() = std::move(joined);
        // 条件不满足，把连接算子放回原位。
        return false;
        // 报告未改写。
    }
    const auto leftSize = join.children[0].output.size();
    // 左侧输出的列数；列编号大于等于它的都属于右侧。
    const auto leftRelations = planRelations(join.children[0]);
    const auto rightRelations = planRelations(join.children[1]);
    std::vector<json> terms, leftTerms, rightTerms, remaining;
    // 全部合取项、能下推左侧的、能下推右侧的、以及留在原地的。
    splitConjuncts(filter.predicate, terms);
    // 把 AND 链拆成一个个最小合取项。
    for (auto& term : terms) {
    // 逐项判断归属。
        if (!safePushdownExpression(term)) { remaining.push_back(std::move(term)); continue; }
        // 该项不安全，留在原过滤里。
        std::set<std::size_t> columns;
        // 该项引用到的列。
        collectColumns(term, columns);
        // 收集列编号。
        if (columns.empty()) { remaining.push_back(std::move(term)); continue; }
        // 一个列都不引用（纯常量条件），无法判断属于哪一侧，留在原地。
        std::set<std::uint32_t> termRelations;
        collectRelations(term, termRelations);
        bool leftOnly = false, rightOnly = false;
        if (!termRelations.empty() && !leftRelations.empty() && !rightRelations.empty()) {
            // 按关系身份判断归属。此前用的是 `id < leftSize`，即左子节点 output
            // 的宽度——一旦有规则裁剪过子节点 output，这个界就不再等于行宽，
            // 判断会静默出错。关系身份与 output 宽度无关。
            leftOnly = std::all_of(termRelations.begin(), termRelations.end(),
                [&](std::uint32_t id) { return leftRelations.contains(id); });
            rightOnly = !leftOnly && std::all_of(termRelations.begin(), termRelations.end(),
                [&](std::uint32_t id) { return rightRelations.contains(id); });
        } else {
            leftOnly = std::all_of(columns.begin(), columns.end(), [&](std::size_t id) { return id < leftSize; });
            rightOnly = std::all_of(columns.begin(), columns.end(), [&](std::size_t id) { return id >= leftSize; });
        }
        if (leftOnly) leftTerms.push_back(std::move(term));
        // 只依赖左表，下推到左侧。
        else if (rightOnly) rightTerms.push_back(shiftColumns(std::move(term), leftSize));
        // 只依赖右表：下推前要把列编号换算成右侧子算子的编号。
        else remaining.push_back(std::move(term));
        // 同时依赖两侧（真正的连接条件），必须留在连接上方，不能下推。
    }
    // 归属判断结束。
    if (leftTerms.empty() && rightTerms.empty()) return false;
    // 一项都下推不了，规则没有效果。
    const auto before = sql::serializePlans({filter});
    // 记下改写前的形态。
    const auto wrap = [](sql::LogicalPlan child, std::vector<json> predicates) {
    // 局部工具：给一个子计划套上一层 Filter，谓词由若干项 AND 而成。
        sql::LogicalPlan pushed;
        // 新建的过滤算子。
        pushed.kind = "Filter";
        // 种类是过滤。
        pushed.table = child.table;
        // 表名沿用孩子的，保证上层按表名做的判断仍然成立。
        pushed.output = child.output;
        // 输出列定义不变，过滤不改变结构。
        pushed.preservesRowId = child.preservesRowId;
        // 行号保持性也沿用，保证上层仍能按物理行号回表。
        pushed.predicate = combineConjuncts(std::move(predicates));
        // 把下推下来的项合成一个谓词。
        pushed.children.push_back(std::move(child));
        // 孩子挂在下面。
        return pushed;
        // 返回包装后的子计划。
    };
    // wrap 定义结束。
    if (!leftTerms.empty()) join.children[0] = wrap(std::move(join.children[0]), std::move(leftTerms));
    // 左下有下推项就在左侧输入上加一层过滤。
    if (!rightTerms.empty()) join.children[1] = wrap(std::move(join.children[1]), std::move(rightTerms));
    // 右侧同理。
    if (remaining.empty()) filter = std::move(joined);
    // 全部项都下推完了，连接上方这层过滤就没有意义了，直接换成连接算子。
    else {
    // 还有留在原地的项。
        filter.children.front() = std::move(joined);
        // 把连接算子放回过滤下方。
        filter.predicate = combineConjuncts(std::move(remaining));
        // 过滤谓词换成剩余项。
    }
    record(changes, "predicate-pushdown", statement, before, sql::serializePlans({filter}));
    // 记录这次下推。
    return true;
    // 报告已改写。
}
void rewritePlan(sql::LogicalPlan& plan, const Options& options, json& changes, std::size_t statement, std::size_t depth = 0, bool selectQuery = false) {
// 对一棵逻辑计划做规则改写；采用自底向上：先改写所有孩子，再处理本节点。
    if (depth > 256) throw MiniSqlError(ErrorCode::Internal, "Optimizer plan depth exceeded");
    // 计划深度保护，避免畸形计划把递归栈打爆。
    const bool childSelectQuery = selectQuery || plan.kind == "Project";
    // 判断孩子是否位于"SELECT 查询"语境：一旦经过 Project，就进入了查询的结果层，
    // 这条信息决定某些规则（如把恒假过滤改写成返回零行）能否安全使用。
    for (auto& child : plan.children) rewritePlan(child, options, changes, statement, depth + 1, childSelectQuery);
    // 先递归改写所有孩子。
    for (auto& expression : plan.projections) expression = rewrite(expression, options, changes, statement);
    // 改写投影表达式。
    for (auto& expression : plan.groupKeys) expression = rewrite(expression, options, changes, statement);
    // 改写分组键表达式。
    for (auto& aggregate : plan.aggregates)
    // 改写聚合函数的参数。
        if (!aggregate.at("argument").is_null()) aggregate["argument"] = rewrite(aggregate.at("argument"), options, changes, statement);
        // COUNT(*) 这类没有参数的聚合写法参数为 null，遇到就跳过。
    for (auto& expression : plan.insertExpressions) expression = rewrite(expression, options, changes, statement);
    // 改写 INSERT 的表达式形式取值。
    for (auto& row : plan.insertRows)
    // 改写 INSERT 多行形式的每一行。
        for (auto& expression : row.at("expressions")) expression = rewrite(expression, options, changes, statement);
        // 行内逐个表达式处理。
    if (plan.kind != "Filter" && plan.kind != "SemiJoin" && plan.kind != "AntiJoin" && plan.kind != "Apply" &&
        // 下面这些规则都只作用于"带谓词的算子"，
        plan.kind != "NestedLoopJoin" && plan.kind != "LeftJoin" && plan.kind != "HashJoin") return;
        // 其余算子（扫描、聚合、排序等）到这里就结束了。
    plan.predicate = rewrite(plan.predicate, options, changes, statement);
    // 改写本节点的谓词表达式。
    if (options.decorrelateSubquery && plan.kind == "Filter" && !plan.subqueryJoinKind.empty()) {
    // 子查询去关联：过滤器上带有 SemiJoin/AntiJoin/Apply 的分类标记时可以提升节点类型。
        record(changes, "decorrelate-subquery", statement, {{"kind", "Filter"}, {"subqueryJoinKind", plan.subqueryJoinKind}},
            // 记录改写前是带分类标记的 Filter，
            {{"kind", plan.subqueryJoinKind}, {"execution", "grouped-parameter-instances"}});
            // 改写后是带类型的子查询节点，执行语义仍是"按绑定参数分组物化"。
        plan.kind = plan.subqueryJoinKind;
        // 真正把种类换掉。
    }
    if (options.hashJoin && chooseJoinAlgorithm(plan, options, changes, statement)) return;
    if (plan.kind != "Filter") return;
    // 剩下三条规则只作用于过滤算子。
    if (options.predicatePushdown && pushPredicateIntoJoin(plan, changes, statement)) return;
    // 先尝试谓词下推；真下推成功就说明本节点已经处理完。
    if (options.removeTrueFilter && boolean(plan.predicate) && plan.predicate.at("value").get<bool>() && plan.children.size() == 1) {
    // 删除恒真过滤：谓词是布尔字面量 true，且只有一个输入。
        const auto& input = plan.children.front();
        // 取出输入算子。
        if (plan.table != input.table || plan.preservesRowId != input.preservesRowId || plan.output.size() != input.output.size()) return;
        // 只有当过滤器的表名、行号保持性与输出列数都和输入完全一致时才允许删除。
        for (std::size_t i = 0; i < plan.output.size(); ++i) {
        // 再逐列比对输出元数据。
            if (plan.output[i].name != input.output[i].name || plan.output[i].type != input.output[i].type ||
                // 列名与类型必须一致，
                plan.output[i].columnId != input.output[i].columnId || plan.output[i].nullable != input.output[i].nullable ||
                // 列编号与可空性也必须一致，
                plan.output[i].defaultValue != input.output[i].defaultValue ||
                // 默认值、主键标记、
                plan.output[i].primaryKey != input.output[i].primaryKey || plan.output[i].unique != input.output[i].unique ||
                // 唯一标记、
                plan.output[i].binding != input.output[i].binding || plan.output[i].relation != input.output[i].relation ||
                plan.output[i].references != input.output[i].references) return;
                // 外键信息，任一不同都不允许删除这层过滤。
        }
        // 元数据比对结束。
        record(changes, "remove-true-filter", statement, {{"kind", "Filter"}}, {{"kind", plan.children.front().kind}});
        // 记录这次删除。
        auto child = std::move(plan.children.front());
        // 把输入算子移到局部变量（先移出，再赋给 plan，避免自赋值问题）。
        plan = std::move(child);
        // 用输入算子整体替换过滤算子。
    }
    if (options.removeFalseFilter && selectQuery && literal(plan.predicate) && (plan.predicate.at("value") == false || plan.predicate.at("value").is_null()) && plan.children.size() == 1) {
    // 删除恒假过滤：谓词是 false 或 NULL，并且当前处于查询结果语境。
        record(changes, "remove-false-filter", statement, {{"kind", "Filter"}, {"predicate", plan.predicate}}, {{"kind", "Limit"}, {"limit", "0"}});
        // 记录改写：过滤算子被替换成"限制输出零行"。
        sql::LogicalPlan limit;
        // 新建限制算子。
        limit.kind = "Limit";
        // 种类是 Limit。
        limit.table = plan.table;
        // 表名沿用。
        limit.output = plan.output;
        // 输出列定义保持不变，保证上层元数据不变。
        limit.limit = 0;
        // 限制为 0 行。
        limit.offset = 0;
        // 偏移为 0。
        limit.children = std::move(plan.children);
        // 把孩子整体搬过去；因为限制是 0 行，孩子实际上不会被求值。
        plan = std::move(limit);
        // 用限制算子替换过滤算子。
    }
}
void checkBudget(const std::vector<sql::LogicalPlan>& plans, std::size_t maximum) {
// 规模预算检查：把计划里所有"可能被外部输入撑大"的内容折算成节点数，
// 一旦超过上限就拒绝优化，避免被构造出来的巨型计划拖垮进程。
    std::size_t nodes = 0;
    // 已经折算出的节点总数。
    std::vector<std::pair<const sql::LogicalPlan*, std::size_t>> pending;
    // 待遍历的计划节点及其深度；用显式栈而不是递归，深度再大也不会爆栈。
    for (const auto& plan : plans) pending.emplace_back(&plan, 0);
    // 把顶层计划逐个入栈。
    auto countJson = [&](const json& root) {
    // 局部工具：把一棵 JSON 表达式树折算成节点数。
        std::vector<std::pair<const json*, std::size_t>> values{{&root, 0}};
        // 待遍历的 JSON 节点及深度。
        while (!values.empty()) {
        // 逐个遍历。
            const auto [value, depth] = values.back(); values.pop_back();
            // 取栈顶元素并出栈。
            if (++nodes > maximum || depth > 512)
            // 累计节点数超过上限，或 JSON 自身嵌套超过 512 层。
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer node/depth budget exceeded");
                // 按参数错误拒绝本次优化。
            if (value->is_structured()) for (const auto& child : *value) values.emplace_back(&child, depth + 1);
            // 数组或对象就把孩子入栈，深度加一。
        }
    };
    // countJson 定义结束。
    while (!pending.empty()) {
    // 逐个处理计划节点。
        const auto [plan, depth] = pending.back(); pending.pop_back();
        // 取栈顶计划节点。
        if (++nodes > maximum || depth > 256 || plan->output.size() > maximum - nodes)
        // 节点数、计划深度、输出列数三处都要检查。
            throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer node/depth budget exceeded");
            // 任一超预算即拒绝。
        nodes += plan->output.size();
        // 输出列计入规模。
        for (const auto& key : plan->keys) {
        // 逐条键约束统计。
            if (++nodes > maximum || key.columns.size() > maximum - nodes)
            // 约束本身算一个节点，其列数也要检查。
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer key budget exceeded");
                // 超预算拒绝。
            nodes += key.columns.size();
            // 累加列数。
        }
        // 键约束统计结束。
        countJson(plan->predicate); countJson(plan->projections); countJson(plan->values); countJson(plan->sortKeys); countJson(plan->insertExpressions); countJson(plan->insertRows);
        // 谓词、投影、值列表、排序键、INSERT 表达式与多行数据逐棵折算。
        countJson(plan->checks); countJson(plan->checkDefinitions);
        // CHECK 表达式与 CHECK 元数据同样折算。
        countJson(plan->groupKeys); countJson(plan->aggregates);
        // 分组键与聚合调用折算。
        if (plan->constraintNames.size() > (maximum - nodes) / 3)
        // 每条约束命名折合 3 个节点（名字、类别、下标）；先做除法避免乘法溢出。
            throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer constraint name budget exceeded");
            // 超预算拒绝。
        nodes += plan->constraintNames.size() * 3;
        // 累加。
        for (const auto& reference : plan->foreignKeys) {
        // 逐条外键统计。
            if (++nodes > maximum || reference.columns.size() > maximum - nodes)
            // 外键本身加它的子表列数。
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer foreign key budget exceeded");
                // 超预算拒绝。
            nodes += reference.columns.size();
            // 累加子表列数。
            if (reference.referencedColumns.size() > maximum - nodes)
            // 父表列数同样要检查。
                throw MiniSqlError(ErrorCode::InvalidArgument, "Optimizer foreign key budget exceeded");
                // 超预算拒绝。
            nodes += reference.referencedColumns.size();
            // 累加父表列数。
        }
        // 外键统计结束。
        for (const auto& child : plan->children) pending.emplace_back(&child, depth + 1);
        // 孩子入栈，深度加一。
    }
    // 全部节点处理完毕，预算检查通过。
}
}
nlohmann::json ruleDescriptors() {
// 返回全部优化规则的描述，供 EXPLAIN 与前端展示"有哪些规则、各自保证什么"。
    return json::array({
    // 结果是数组，每个元素描述一条规则；precondition 是适用前提，postcondition 是语义保证。
        {{"ruleId", "constant-arithmetic"}, {"scope", "expression"}, {"precondition", "INT32 constant operands; successful checked arithmetic"}, {"postcondition", "Same INT32 value; errors remain unevaluated"}},
        // 常量算术折叠：前提是 32 位整数常量且运算通过溢出检查；
        // 保证结果值不变，并且原本会抛错的表达式仍然不会被提前求值。
        {{"ruleId", "constant-comparison"}, {"scope", "expression"}, {"precondition", "Semantically bound constant operands of compatible types, including NULL"}, {"postcondition", "Same BOOL comparison result, including UNKNOWN"}},
        // 常量比较折叠：前提是两侧类型兼容的常量（含 NULL）；
        // 保证比较结果一致，包括结果为 UNKNOWN 的情况。
        {{"ruleId", "boolean-simplification"}, {"scope", "expression"}, {"precondition", "BOOL/NULL constants or literal null tests; preserve left-to-right short circuit and possible errors"}, {"postcondition", "Same three-valued BOOL result and evaluation errors"}},
        // 布尔化简：前提是布尔/NULL 常量或对常量的空值判断；
        // 保证三值逻辑结果与错误行为都不变。
        {{"ruleId", "remove-true-filter"}, {"scope", "plan"}, {"precondition", "One child, same table/schema/RowId contract and literal BOOL true predicate"}, {"postcondition", "Preserve child schema, order, duplicates and RowId"}},
        // 删除恒真过滤：前提是单输入、表与元数据契约一致、谓词为 true；
        // 保证输出结构、行顺序、重复行与行号保持性都不变。
        {{"ruleId", "remove-false-filter"}, {"scope", "plan"}, {"precondition", "One child and literal FALSE or NULL predicate"}, {"postcondition", "Return no rows with unchanged output schema and do not execute child"}},
        // 删除恒假过滤：前提是单输入、谓词为 false 或 NULL；
        // 保证返回零行、输出结构不变，并且不再执行子计划。
        {{"ruleId", "predicate-pushdown"}, {"scope", "plan"}, {"precondition", "INNER join Filter with side-local, side-effect-free comparison/boolean predicates"}, {"postcondition", "Same rows, NULL results and error timing for safe predicates"}},
        // 谓词下推：前提是内连接上的过滤，且谓词只依赖单侧、无副作用；
        // 保证行集、NULL 语义与错误时机都不变。
        {{"ruleId", "hash-join"}, {"scope", "plan"}, {"precondition", "INNER NestedLoopJoin with direct left/right column equality"}, {"postcondition", "Same inner-join rows, duplicates and NULL non-matching semantics"}},
        // 哈希连接：前提是内连接且谓词为左右列的直接等值比较；
        // 保证连接结果、重复行与 NULL 不匹配语义一致。
        {{"ruleId", "prune-columns"}, {"scope", "plan"}, {"precondition", "Project over SeqScan with optional single Filter; projections are explicit"}, {"postcondition", "Scan output metadata contains exactly referenced columns; row values and errors unchanged"}}
        // 列裁剪：前提是投影直接架在扫描上（中间可有一层过滤）且投影是显式写出的；
        // 保证扫描输出只保留被引用的列，行值与错误不变。
        ,{{"ruleId", "decorrelate-subquery"}, {"scope", "plan"}, {"precondition", "Filter carries SemiJoin/AntiJoin/Apply subquery classification"}, {"postcondition", "Promote to typed subquery node while preserving grouped-parameter execution semantics"}}
        // 子查询去关联：前提是过滤器带有 semi/anti/apply 分类标记；
        // 保证提升为带类型的子查询节点后，按绑定参数分组执行的语义不变。
    });
    // 数组结束。
}
Result optimize(const std::vector<sql::LogicalPlan>& plans, Options options) {
// 优化主入口：以"迭代到不动点"的方式反复应用全部规则，并全程守住预算。
    if (options.maxIterations == 0 || options.maxIterations > 64 || options.maxNodes == 0 || options.maxNodes > 1000000)
    // 迭代上限必须在 1..64，节点上限必须在 1..1000000。
        throw MiniSqlError(ErrorCode::InvalidArgument, "Invalid optimizer iteration/node budget");
        // 超出范围按参数错误拒绝，避免调用方传进来一个近乎无限循环的配置。
    for (const auto& id : options.disabledRules) {
    // 处理用户显式要求禁用的规则名。
        if (id == "constant-arithmetic") options.constantArithmetic = false;
        // 逐个名字映射到对应开关。
        else if (id == "constant-comparison") options.constantComparison = false;
        // 常量比较。
        else if (id == "boolean-simplification") options.booleanSimplification = false;
        // 布尔化简。
        else if (id == "remove-true-filter") options.removeTrueFilter = false;
        // 删除恒真过滤。
        else if (id == "remove-false-filter") options.removeFalseFilter = false;
        // 删除恒假过滤。
        else if (id == "predicate-pushdown") options.predicatePushdown = false;
        // 谓词下推。
        else if (id == "hash-join") options.hashJoin = false;
        // 哈希连接。
        else if (id == "prune-columns") options.pruneColumns = false;
        // 列裁剪。
        else if (id == "decorrelate-subquery") options.decorrelateSubquery = false;
        // 子查询去关联。
        else throw MiniSqlError(ErrorCode::InvalidArgument, "Unknown optimizer rule: " + id);
        // 不认识的名字直接报错，避免用户以为禁用生效了其实没有。
    }
    // 规则开关处理结束。
    checkBudget(plans, options.maxNodes);
    // 改写之前先检查一次预算。
    Result result{plans, json::array()};
    // 结果从"优化前的计划"开始，改动记录为空。
    auto previous = sql::serializePlans(result.plans).dump();
    // 把当前计划序列化成文本，作为"上一轮形态"的比较基准。
    std::set<std::string> seen{previous};
    // 记录见过的所有计划形态，用于检测规则互相激发形成环。
    for (std::size_t iteration = 1; iteration <= options.maxIterations; ++iteration) {
    // 在迭代上限内反复应用规则。
        const auto start = result.changes.size();
        // 记下本轮开始时的改动条数，便于给本轮新增改动打轮次标记。
        for (std::size_t i = 0; i < result.plans.size(); ++i) {
            rewritePlan(result.plans[i], options, result.changes, i);
            if (options.pruneColumns) pruneRequiredColumns(result.plans[i], result.changes, i);
        }
        checkBudget(result.plans, options.maxNodes);
        // 每轮之后重新检查预算：规则可能把计划变复杂。
        result.iterations = iteration;
        // 记录已经跑过的轮数。
        for (std::size_t i = start; i < result.changes.size(); ++i) {
        // 给本轮新增的改动补上轮次与序号。
            result.changes[i]["iteration"] = iteration;
            // 属于第几轮。
            result.changes[i]["sequence"] = i;
            // 全局第几条改动。
        }
        // 标记结束。
        const auto current = sql::serializePlans(result.plans).dump();
        // 序列化本轮结束后的计划。
        if (current == previous) { result.converged = true; return result; }
        // 与上一轮完全相同，说明到达不动点：标记收敛并返回。
        if (!seen.insert(current).second) {
        // 这个形态以前出现过，说明规则之间形成了环。
            result.diagnostics.push_back({{"code", "OPTIMIZER_CYCLE"}, {"message", "Repeated plan structure; optimization stopped"}});
            // 记录一条"检测到循环"的诊断。
            return result;
            // 立即停止，避免在同一组形态之间来回改写。
        }
        // 第一次见到这个形态，继续下一轮。
        previous = current;
        // 更新比较基准。
    }
    // 用完全部迭代轮数仍未收敛。
    result.diagnostics.push_back({{"code", "OPTIMIZER_ITERATION_LIMIT"}, {"message", "Iteration budget reached; convergence not proven"}});
    // 明确记录"只是用完了轮数，并未证明收敛"，而不是假装成功。
    return result;
}
}
