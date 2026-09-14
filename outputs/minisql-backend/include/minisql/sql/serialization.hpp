#pragma once
#include "minisql/sql/parser.hpp"
#include "minisql/common/decimal_type.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/varchar.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>

namespace minisql::sql {
// X13 版本契约。下面这些是线上传输与落盘序列化产物的权威版本常量。
// 读取时如果主版本号不认识就直接拒绝；主版本相同但次版本更高的产物，
// 在字段存在的前提下要按宽容策略读取。
// X13 version contract. These are the authoritative constants for on-wire and
// on-disk serialized artifacts. Unknown major versions are rejected on read;
// same major, higher minor must be read leniently where fields are present.
inline constexpr std::uint32_t AST_SCHEMA_VERSION = 1;      // AST document schemaVersion
// 语法树文档的架构版本号，改动 AST 字段含义时必须提升它。
inline constexpr std::uint32_t PLAN_SCHEMA_VERSION = 1;     // logical plan schemaVersion
// 逻辑计划文档的架构版本号。
inline constexpr std::uint32_t PRODUCER_VERSION = 1;        // this binary's producer version
// 生成这些产物的程序版本，便于排查"是哪个版本写出来的"。
inline constexpr std::uint32_t CATALOG_SCHEMA_VERSION = 6;  // numeric persisted column typeId
inline nlohmann::json serializeReference(const std::optional<std::pair<std::string,std::string>>& reference) {
// 把列级外键（父表名，父列名）序列化成 JSON。
    return reference ? nlohmann::json{{"table", reference->first}, {"column", reference->second}} : nlohmann::json(nullptr);
    // 有外键就输出对象形式；没有就用 JSON 的 null 表示，保持字段始终存在。
}
inline nlohmann::json serializeKeys(const std::vector<KeyConstraint>& keys) {
// 把主键/唯一键约束列表序列化成 JSON 数组。
    auto result = nlohmann::json::array();
    // 结果数组。
    for (const auto& key : keys) result.push_back({{"primary", key.primary}, {"columns", key.columns}});
    // 每条约束输出两个字段：是不是主键、由哪些列组成。
    return result;
    // 返回数组。
}
inline nlohmann::json serializeForeignKeys(const std::vector<ForeignKey>& keys) {
// 把表级外键列表序列化成 JSON 数组。
    auto result = nlohmann::json::array();
    // 结果数组。
    for (const auto& key : keys) result.push_back({{"columns", key.columns}, {"table", key.table}, {"referencedColumns", key.referencedColumns}});
    // 每条外键输出本表列、父表名、父表列三部分。
    return result;
    // 返回数组。
}
inline nlohmann::json serializeConstraintNames(const std::vector<ConstraintName>& names) {
// 把"约束名字绑定"列表序列化成 JSON 数组。
    auto result = nlohmann::json::array();
    // 结果数组。
    for (const auto& binding : names) result.push_back({{"name", binding.name}, {"kind", binding.kind}, {"index", binding.index}});
    // 每条绑定输出名字、约束类别、目标下标三部分，反序列化时按同结构还原。
    return result;
    // 返回数组。
}
inline nlohmann::json serializeExpression(const std::shared_ptr<Expr>& expression, std::size_t depth = 0) {
// 把表达式树递归序列化成 JSON；depth 记录当前递归深度。
    if (!expression) return nullptr;
    // 空节点直接写成 null（例如没有 WHERE 条件）。
    if (depth > 256) throw MiniSqlError(ErrorCode::Syntax, "Expression depth exceeded");
    // 深度保护：超过 256 层说明表达式畸形，直接抛出而不让递归栈爆掉。
    nlohmann::json node = {{"kind", expression->kind}, {"value", expression->value},
        {"line", expression->location.line}, {"column", expression->location.column}};
    // 每个节点都写出四个基础字段：种类、文本值、以及源位置的行列号。
    if (!expression->subquerySql.empty()) node["subquerySql"] = expression->subquerySql;
    // 只有当节点携带子查询原文时才写这个字段，避免产物里塞满空串。
    if (expression->left) node["left"] = serializeExpression(expression->left, depth + 1);
    // 有左子节点就递归序列化，并把深度加一。
    if (expression->right) node["right"] = serializeExpression(expression->right, depth + 1);
    // 有右子节点同样递归处理。
    return node;
    // 返回这个节点的 JSON 表示。
}
namespace detail {
inline std::shared_ptr<Expr> readCheckExpression(const nlohmann::json& node, std::size_t depth, std::size_t& remaining) {
// 把一个 JSON 节点严格还原成表达式；remaining 是本次解析剩余的节点预算。
    auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); };
    // 局部工具：任何校验失败统一抛同一类存储错误，避免调用方区分不同异常。
    if (!node.is_object() || depth > 256 || remaining == 0) invalid();
    // 三项前置检查：必须是对象、深度不能超 256、节点预算还没用完。
    --remaining;
    // 消耗一个节点预算，防止超大 JSON 造成资源耗尽。
    for (const auto* field : {"kind", "value"})
    // 两个必填字符串字段。
        if (!node.contains(field) || !node.at(field).is_string()) invalid();
        // 缺字段或类型不对都判为非法产物。
    for (const auto* field : {"line", "column"}) {
    // 两个必填的整数位置字段。
        if (!node.contains(field) || !node.at(field).is_number_integer()) invalid();
        // 缺字段或不是整数都非法。
        const auto& position = node.at(field);
        // 取出该字段的值。
        if (!position.is_number_unsigned() && position.get<std::int64_t>() < 0) invalid();
        // 若是有符号形式，不允许出现负数行列号。
        if (position.get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) invalid();
        // 也不能超出本机 size_t 能表示的范围，否则后面转换会截断。
    }
    auto expression = std::make_shared<Expr>();
    // 校验通过，开始构造表达式节点。
    expression->kind = node.at("kind").get<std::string>();
    // 还原节点种类。
    expression->value = node.at("value").get<std::string>();
    // 还原节点文本值。
    expression->location = {node.at("line").get<std::size_t>(), node.at("column").get<std::size_t>()};
    // 还原源位置。
    const auto& kind = expression->kind;
    // 后面反复用到种类名，取个短别名。
    const bool subqueryKind = kind == "InSubquery" || kind == "Exists" || kind == "ScalarSubquery";
    // 判断这是不是三种"带子查询"的节点之一，它们的校验规则与普通节点不同。
    auto value = expression->value;
    // 复制一份文本值用于规范化比较。
    if (!subqueryKind && (value.empty() || value.size() > 1048576)) invalid();
    // 普通节点的文本值不能为空，也不能超过 1 MiB，防止有人塞超长垃圾。
    for (char& c : value) if (c >= 'a' && c <= 'z') c -= 32;
    // 把小写字母整体转大写，用于与规范化的运算符/函数名比较。
    const bool binary = kind == "Binary";
    // 是不是二元运算节点。
    const bool unary = kind == "Unary" || kind == "Cast";
    // 是不是一元运算或类型转换节点（两者结构都是"一个子节点"）。
    if ((binary || kind == "Unary") && expression->value != value) invalid();
    // 二元与 Unary 节点的 value 必须已经是大写规范形式，否则说明产物被人手改过。
    if (!binary && !unary && kind != "Literal" && kind != "Identifier" && !subqueryKind) invalid();
    // 种类白名单：只允许这几种，别的种类名一律拒绝。
    if (subqueryKind) {
    // 子查询类节点的专属校验。
        if (!node.contains("subquerySql") || !node.at("subquerySql").is_string()) invalid();
        // 必须携带子查询原文，否则后续没法核对。
        expression->subquerySql = node.at("subquerySql").get<std::string>();
        // 还原子查询原文。
        if (expression->subquerySql.empty() || expression->subquerySql.size() > 1048576) invalid();
        // 原文也不能为空或超长。
    }
    const std::size_t expectedSize = binary ? 6u : unary ? 5u : subqueryKind ? (kind == "InSubquery" ? 6u : 5u) : 4u;
    // 按节点种类算出"这个 JSON 对象应该恰好有几个字段"，多一个少一个都说明产物不合法。
    const auto metadataSize = static_cast<std::size_t>(node.contains("nodeId")) + static_cast<std::size_t>(node.contains("sourceSpan"));
    if (node.size() != expectedSize + metadataSize ||
        node.contains("left") != (binary || unary || kind == "InSubquery") || node.contains("right") != binary) invalid();
    // 字段个数要精确匹配；而且"有 left/right 字段"这件事本身也要和节点结构一致。
    if (node.contains("nodeId") && !node.at("nodeId").is_number_unsigned()) invalid();
    if (node.contains("sourceSpan") && (!node.at("sourceSpan").is_object() ||
        !node.at("sourceSpan").contains("start") || !node.at("sourceSpan").contains("end"))) invalid();
    if (binary && value != "AND" && value != "OR" && value != "=" && value != "!=" &&
        value != "<" && value != "<=" && value != ">" && value != ">=" &&
        value != "+" && value != "-" && value != "*" && value != "/") invalid();
    // 二元运算符白名单：逻辑运算、六种比较运算、四种算术运算。
    if (kind == "Unary" && value != "NOT" && value != "IS NULL" && value != "IS NOT NULL" && value != "+" && value != "-") invalid();
    // 一元运算符白名单。
    if (kind == "Cast" && value != "INT" && value != "BIGINT" && value != "FLOAT" && value != "VARCHAR" && value != "BOOL" && value != "DATE") {
    // CAST 的目标类型不在简单类型里，还要再试试带参数的类型。
        auto type = value;
        // 复制一份类型名。
        for (char& c : type) if (c >= 'A' && c <= 'Z') c += 32;
        // 转成小写，便于匹配 decimal(p,s) 与 varchar(n)。
        if (!minisql::decimalType(type) && !minisql::varcharLength(type)) invalid();
        // 既不是定点数也不是变长字符串，说明类型名不合法。
    }
    if (!binary && !unary && !subqueryKind) {
    // 剩下的是 Literal 与 Identifier 两种叶子节点，要用词法分析器反向核对文本。
        const auto tokens = tokenize(expression->value);
        // 把文本值重新切 token。
        std::string joined;
        // 用于把 token 拼回去做等值比较。
        for (const auto& token : tokens) joined += token.lexeme;
        // 把 token 文本顺序拼起来（注意不加空格，因为叶子节点的值本来就不含空格）。
        if (joined != expression->value) invalid();
        // 拼回来必须与原值完全一致，否则说明这段文本不是合法 token 序列。
        if (kind == "Identifier") {
        // 列引用的合法形态。
            if (!((tokens.size() == 2 && tokens[0].type == "IDENTIFIER") ||
                (tokens.size() == 4 && tokens[0].type == "IDENTIFIER" && tokens[1].lexeme == "." && tokens[2].type == "IDENTIFIER"))) invalid();
            // 只有两种写法合法：单个标识符，或者"标识符 . 标识符"的限定列名。
        } else {
        // 字面量的合法形态。
            const bool single = tokens.size() == 2 && (tokens[0].type == "INTEGER" || tokens[0].type == "DECIMAL" || tokens[0].type == "FLOAT" || tokens[0].type == "STRING" || value == "NULL" || value == "TRUE" || value == "FALSE");
            // 形态一：无符号数字或字符串，或 NULL/TRUE/FALSE 三个关键字。
            const bool signedInteger = tokens.size() == 3 && (tokens[0].lexeme == "+" || tokens[0].lexeme == "-") && (tokens[1].type == "INTEGER" || tokens[1].type == "DECIMAL" || tokens[1].type == "FLOAT");
            // 形态二：正负号加数字。
            const bool dateLiteral = tokens.size() == 3 && value.starts_with("DATE'") && tokens[1].type == "STRING";
            // 形态三：DATE 关键字加字符串。
            if (dateLiteral) { const auto date = dateLiteralText(value);if (!date) invalid();(void)parseIsoDate(*date); }
            // 日期字面量还要真的解析一次：格式不对或者不是有效日期都要拒绝。
            if (!single && !signedInteger && !dateLiteral) invalid();
            // 三种形态都不满足，判为非法字面量。
        }
    }
    if (binary || unary || kind == "InSubquery") expression->left = readCheckExpression(node.at("left"), depth + 1, remaining);
    // 需要左子节点的种类就递归还原左子树；因为前面已校验 left 字段存在，这里可以放心取。
    if (binary) expression->right = readCheckExpression(node.at("right"), depth + 1, remaining);
    // 二元节点还要还原右子树。
    return expression;
    // 返回完整还原并校验过的表达式。
}
}
inline std::shared_ptr<Expr> deserializeExpression(const nlohmann::json& node, std::size_t depth = 0) {
// 对外入口：把 JSON 还原成表达式，并把内部异常统一成同一种存储错误。
    std::size_t remaining = 65536;
    // 给这次解析 65536 个节点的预算，防止超大文档拖垮内存。
    try { return detail::readCheckExpression(node, depth, remaining); }
    // 正常路径走内部实现。
    catch (const MiniSqlError&) { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); }
    // 内部校验失败时统一改写成同一条存储错误，不泄露细节。
    catch (const nlohmann::json::exception&) { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized CHECK expression"); }
    // JSON 取值类型不符也会抛异常，同样统一改写，保证调用方只处理一种错误。
}
inline nlohmann::json serializeChecks(const std::vector<std::shared_ptr<Expr>>& checks) {
// 把一组表达式（CHECK 条件、GROUP BY 分组键都用它）序列化成 JSON 数组。
    auto result = nlohmann::json::array();
    // 结果数组。
    for (const auto& check : checks) result.push_back(serializeExpression(check));
    // 逐个表达式递归序列化后追加，顺序保持原样。
    return result;
    // 返回数组。
}
inline nlohmann::json serializeStatement(const Statement& statement) {
// 把一条语句的整棵语法树序列化成 JSON 对象。
    using json = nlohmann::json;
    // 引入短别名，避免反复写完整命名空间。
    json node = {{"kind", statement.kind}, {"table", statement.table}, {"tableAlias", statement.tableAlias}, {"indexName", statement.indexName}, {"uniqueIndex", statement.uniqueIndex}, {"indexColumns", statement.indexColumns}, {"selectList", statement.selectList},
        {"names", statement.names}, {"values", statement.values}, {"where", serializeExpression(statement.where)},
        {"line", statement.location.line}, {"column", statement.location.column},
        {"columns", json::array()}, {"selectItems", json::array()}, {"orderBy", json::array()},
        {"distinct", statement.distinct}, {"limit", statement.limit ? json(std::to_string(*statement.limit)) : json(nullptr)},
        {"offset", std::to_string(statement.offset)}};
    // 先写语句的标量字段：种类、表名、别名、索引相关字段、列名表、值表、WHERE 表达式、
    // 语句起始行列号、是否去重，以及 LIMIT/OFFSET；columns/selectItems/orderBy 先占位成空数组。
    // LIMIT 与 OFFSET 故意转成字符串：大整数放进 JSON number 可能被浮点截断，
    // 用字符串承载才能保证 64 位整数按位还原。
    for (const auto& column : statement.columns)
    // 逐列序列化建表列定义。
        node["columns"].push_back({{"name", column.name}, {"type", column.type}, {"nullable", column.nullable},
            {"defaultValue", column.defaultValue ? json(*column.defaultValue) : json(nullptr)}, {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", serializeReference(column.references)}});
        // 每列输出名字、类型、可空性、默认值（没有就是 null）、主键/唯一标记、列级外键。
    for (const auto& item : statement.selectItems)
    // 逐个序列化投影项。
        node["selectItems"].push_back({{"expression", serializeExpression(item.expression)}, {"alias", item.alias}});
        // 每项输出表达式子树与别名（没写别名就是空串）。
    for (const auto& item : statement.orderBy)
    // 逐个序列化排序键。
        node["orderBy"].push_back({{"expression", serializeExpression(item.expression)}, {"descending", item.descending}, {"nullsFirst", item.nullsFirst ? json(*item.nullsFirst) : json(nullptr)}});
        // 每项输出排序表达式、是否降序、以及 NULLS FIRST/LAST 的显式设置（未写为 null）。
    node["assignments"] = json::array();
    // 赋值列表先置空数组，稍后填充，保持字段顺序稳定便于人工比对产物。
    node["groupBy"] = serializeChecks(statement.groupBy);
    // GROUP BY 分组键复用表达式数组序列化。
    node["having"] = serializeExpression(statement.having);
    // HAVING 条件（没有就是 null）。
    node["valueExpressions"] = json::array();
    // 表达式形式的值列表先置空，稍后填充。
    node["defaultValues"] = statement.defaultValues;
    // 是否写了 INSERT ... DEFAULT VALUES。
    node["valueRows"] = json::array();
    // 多行插入的行列表先置空。
    for (const auto& row : statement.valueRows) {
    // 逐行序列化多行插入。
        auto values = json::array();
        // 本行的表达式数组。
        for (const auto& expression : row) values.push_back(serializeExpression(expression));
        // 逐个把这一行里的表达式序列化后追加。
        node["valueRows"].push_back(std::move(values));
        // 把整行收进行列表（用 move 避免再复制一次数组）。
    }
    node["keys"] = serializeKeys(statement.keys);
    // 主键/唯一键约束列表。
    node["foreignKeys"] = serializeForeignKeys(statement.foreignKeys);
    // 表级外键列表。
    node["constraintNames"] = serializeConstraintNames(statement.constraintNames);
    // 约束名绑定列表。
    node["checks"] = serializeChecks(statement.checks);
    // CHECK 条件列表（列级与表级合并后的顺序）。
    for (const auto& expression : statement.valueExpressions)
    // 逐项序列化表达式形式的值。
        node["valueExpressions"].push_back(serializeExpression(expression));
        // 追加到前面预留的数组里。
    for (const auto& item : statement.assignments)
    // 逐项序列化 UPDATE 赋值。
        node["assignments"].push_back({{"column", item.column}, {"expression", serializeExpression(item.expression)}});
        // 每项输出目标列名与赋值表达式。
    node["joins"] = json::array();
    // 连接列表先置空。
    for (const auto& join : statement.joins)
    // 逐个序列化 JOIN 子句。
        node["joins"].push_back({{"kind", join.left && join.right ? "FullJoin" : join.left ? "LeftJoin" : join.right ? "RightJoin" : "InnerJoin"}, {"table", join.table}, {"alias", join.alias}, {"on", serializeExpression(join.on)}});
        // 两个布尔标记被压成一个可读的连接类型字符串：同时置位是 FullJoin，
        // 只左是 LeftJoin，只右是 RightJoin，都不置位是 InnerJoin，
        // 这样反序列化时按一个字段就能还原出原来的两个布尔值。
    node["fromSubquery"] = statement.fromSubquery ? serializeStatement(*statement.fromSubquery) : nullptr;
    // FROM 派生表：递归序列化内层 SELECT；没有派生表就写 null。
    return node;
    // 返回整条语句的 JSON 表示。
}
namespace detail {
inline nlohmann::json astSourceSpan(const nlohmann::json& node) {
    const auto line = node.value("line", std::size_t{0});
    const auto column = node.value("column", std::size_t{0});
    const auto endLine = node.value("endLine", line);
    auto endColumn = node.value("endColumn", column);
    if (endColumn == column && node.contains("value") && node.at("value").is_string())
        endColumn += std::max<std::size_t>(1, node.at("value").get_ref<const std::string&>().size());
    if (endColumn == column && line != 0) ++endColumn;
    return {{"start", {{"line", line}, {"column", column}}},
            {"end", {{"line", endLine}, {"column", endColumn}}}};
}
inline void annotateAst(nlohmann::json& node, std::size_t& nextId) {
    if (node.is_array()) {
        for (auto& child : node) annotateAst(child, nextId);
        return;
    }
    if (!node.is_object()) return;
    // 判定"这是不是一个 AST 节点"不能只看 kind：constraintNames 的元素也带 kind
    // （取值是 key/check/foreignKey 这类约束类别），若一并标注就会在 ast.constraintNames
    // 里凭空多出 nodeId/sourceSpan，与 tests/named-constraint-process.mjs 的期望不符。
    // 真正的 AST/表达式节点一定带源位置（line/column），用这个特征把元数据排除掉。
    if (node.contains("kind") && node.at("kind").is_string() && node.contains("line") && node.contains("column")) {
        node["nodeId"] = nextId++;
        node["sourceSpan"] = astSourceSpan(node);
        if (node.contains("selectItems")) {
            auto schema = nlohmann::json::array();
            for (const auto& item : node.at("selectItems"))
                schema.push_back({{"name", item.value("alias", std::string{})}, {"type", "unknown"}});
            node["outputSchema"] = std::move(schema);
        }
    }
    for (auto& child : node.items())
        if (child.key() != "sourceSpan" && child.key() != "outputSchema") annotateAst(child.value(), nextId);
}
inline Statement readStatement(const nlohmann::json& node, std::size_t depth = 0) {
// 把一个 JSON 对象严格还原成 Statement；depth 用于限制嵌套深度。
    auto invalid = []() -> void { throw MiniSqlError(ErrorCode::Storage, "Invalid serialized AST statement"); };
    // 局部工具：所有校验失败统一抛同一条存储错误。
    if (!node.is_object() || depth > 64) invalid();
    // 必须是对象，并且嵌套不能超过 64 层（控制派生表与子查询的递归深度）。
    const auto stringField = [&](const char* name) -> std::string {
    // 局部工具：读取一个必填的字符串字段。
        if (!node.contains(name) || !node.at(name).is_string()) invalid();
        // 字段缺失或者不是字符串就判为非法。
        return node.at(name).get<std::string>();
        // 返回该字符串。
    };
    const auto strings = [&](const char* name) -> std::vector<std::string> {
    // 局部工具：读取一个必填的字符串数组字段。
        if (!node.contains(name) || !node.at(name).is_array()) invalid();
        // 字段缺失或者不是数组就判为非法。
        std::vector<std::string> result;
        // 结果容器。
        for (const auto& value : node.at(name)) { if (!value.is_string()) invalid(); result.push_back(value.get<std::string>()); }
        // 逐元素校验类型并取出，任何一个不是字符串都判为非法。
        return result;
        // 返回还原出的字符串数组。
    };
    const auto expression = [&](const char* name) -> std::shared_ptr<Expr> {
    // 局部工具：读取一个可选的表达式字段。
        if (!node.contains(name) || node.at(name).is_null()) return {};
        // 字段不存在或者是 null，都表示"没有这个表达式"，返回空指针。
        return deserializeExpression(node.at(name), depth + 1);
        // 否则递归反序列化，并把深度加一。
    };
    Statement statement;
    // 开始构造语句对象。
    statement.kind = stringField("kind");
    // 还原语句种类。
    statement.table = stringField("table");
    // 还原主表名。
    statement.tableAlias = node.value("tableAlias", std::string{});
    // 还原表别名；老版本产物可能没有这个字段，缺失时按空串处理（向后兼容）。
    statement.indexName = node.value("indexName", std::string{});
    // 还原索引名，缺失按空串。
    statement.uniqueIndex = node.value("uniqueIndex", false);
    // 还原是否唯一索引，缺失按 false。
    statement.indexColumns = node.value("indexColumns", std::vector<std::string>{});
    // 还原索引列，缺失按空列表。
    statement.selectList = strings("selectList");
    // 还原兼容用的投影文本列表。
    statement.names = strings("names");
    // 还原列名列表。
    statement.values = strings("values");
    // 还原字面量文本值列表。
    statement.where = expression("where");
    // 还原 WHERE 表达式。
    statement.location = {node.value("line", std::size_t{0}), node.value("column", std::size_t{0})};
    // 还原语句起始位置；老产物缺字段时退化为 0 行 0 列。
    statement.distinct = node.value("distinct", false);
    // 还原是否 DISTINCT。
    if (node.contains("limit") && !node.at("limit").is_null()) {
    // LIMIT 是可选字段。
        if (!node.at("limit").is_string()) invalid();
        // 前面序列化时约定用字符串承载大整数，这里必须也是字符串。
        std::uint64_t value{};
        // 接收解析结果。
        const auto text = node.at("limit").get<std::string>();
        // 取出文本。
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        // 文本转无符号整数。
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
        // 溢出或者没读完整个文本都判为非法。
        statement.limit = value;
        // 还原 LIMIT（这里才真正置成有值状态，与"没写 LIMIT"区分开）。
    }
    if (node.contains("offset")) {
    // OFFSET 是可选字段。
        if (!node.at("offset").is_string()) invalid();
        // 同样要求字符串形式。
        const auto text = node.at("offset").get<std::string>();
        // 取出文本。
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), statement.offset);
        // 直接解析到目标字段里。
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid();
        // 溢出或不完整都非法。
    }
    if (node.contains("columns")) for (const auto& item : node.at("columns")) {
    // 逐项还原列定义。
        if (!item.is_object() || !item.contains("name") || !item.contains("type")) invalid();
        // 每列至少要有名字和类型。
        ColumnDef column{item.at("name").get<std::string>(), item.at("type").get<std::string>()};
        // 构造列定义，先填名字与类型。
        column.nullable = item.value("nullable", true);
        // 还原可空性，缺字段按"允许为 NULL"处理。
        column.primaryKey = item.value("primaryKey", false);
        // 还原主键标记。
        column.unique = item.value("unique", false);
        // 还原唯一标记。
        if (item.contains("defaultValue") && !item.at("defaultValue").is_null()) column.defaultValue = item.at("defaultValue").get<std::string>();
        // 默认值可选，非 null 时才还原。
        if (item.contains("references") && !item.at("references").is_null()) {
        // 列级外键可选。
            if (!item.at("references").is_object()) invalid();
            // 有值就必须是对象。
            column.references = std::make_pair(item.at("references").at("table").get<std::string>(), item.at("references").at("column").get<std::string>());
            // 还原成"父表名，父列名"这一对。
        }
        statement.columns.push_back(std::move(column));
        // 把还原好的列收进列表。
    }
    if (node.contains("selectItems")) for (const auto& item : node.at("selectItems")) {
    // 逐项还原投影项。
        if (!item.is_object()) invalid();
        // 每项必须是对象。
        statement.selectItems.push_back({item.contains("expression") ? deserializeExpression(item.at("expression"), depth + 1) : std::shared_ptr<Expr>{}, item.value("alias", std::string{})});
        // 表达式字段存在就递归还原，否则用空指针；别名缺字段按空串。
    }
    if (node.contains("orderBy")) for (const auto& item : node.at("orderBy")) {
    // 逐项还原排序键。
        if (!item.is_object() || !item.contains("expression")) invalid();
        // 每项必须是对象且必须有表达式。
        OrderItem order{deserializeExpression(item.at("expression"), depth + 1), item.value("descending", false)};
        // 还原排序表达式与升降序标志。
        if (item.contains("nullsFirst") && !item.at("nullsFirst").is_null()) order.nullsFirst = item.at("nullsFirst").get<bool>();
        // NULLS FIRST/LAST 是可选项，非 null 时才还原成有值状态。
        statement.orderBy.push_back(std::move(order));
        // 收进排序键列表。
    }
    if (node.contains("assignments")) for (const auto& item : node.at("assignments")) {
    // 逐项还原 UPDATE 赋值。
        if (!item.is_object() || !item.contains("column") || !item.contains("expression")) invalid();
        // 赋值项必须有列名与表达式。
        statement.assignments.push_back({item.at("column").get<std::string>(), deserializeExpression(item.at("expression"), depth + 1)});
        // 还原成一条赋值记录。
    }
    if (node.contains("joins")) for (const auto& item : node.at("joins")) {
    // 逐项还原 JOIN 子句。
        if (!item.is_object() || !item.contains("table") || !item.contains("on")) invalid();
        // 连接必须有被连接的表名和 ON 条件。
        Join join;
        // 准备连接结构。
        join.table = item.at("table").get<std::string>();
        // 还原被连接的表名。
        join.alias = item.value("alias", std::string{});
        // 还原别名，缺字段按空串。
        const auto kind = item.value("kind", "InnerJoin");
        // 取连接类型字符串，缺字段时按内连接处理（兼容只写 kind 的老格式）。
        join.left = kind == "LeftJoin" || kind == "FullJoin";
        // 左外与全外连接都满足"左侧保留"这一条。
        join.right = kind == "RightJoin" || kind == "FullJoin";
        // 右外与全外连接都满足"右侧保留"这一条。
        join.on = deserializeExpression(item.at("on"), depth + 1);
        // 还原 ON 条件表达式。
        statement.joins.push_back(std::move(join));
        // 收进连接列表。
    }
    if (node.contains("fromSubquery") && !node.at("fromSubquery").is_null())
    // FROM 派生表是可选项。
        statement.fromSubquery = std::make_shared<Statement>(readStatement(node.at("fromSubquery"), depth + 1));
        // 递归还原内层 SELECT，并共享持有它。
    if (node.contains("valueExpressions")) for (const auto& item : node.at("valueExpressions")) statement.valueExpressions.push_back(deserializeExpression(item, depth + 1));
    // 逐项还原表达式形式的值列表。
    statement.defaultValues = node.value("defaultValues", false);
    // 还原是否 DEFAULT VALUES 写法。
    if (node.contains("valueRows")) for (const auto& row : node.at("valueRows")) {
    // 逐行还原多行插入。
        if (!row.is_array()) invalid();
        // 每一行必须是数组。
        std::vector<std::shared_ptr<Expr>> values;
        // 本行的表达式序列。
        for (const auto& item : row) values.push_back(deserializeExpression(item, depth + 1));
        // 逐项递归还原。
        statement.valueRows.push_back(std::move(values));
        // 把整行收进行列表。
    }
    if (node.contains("keys")) for (const auto& item : node.at("keys")) {
    // 逐项还原键约束。
        if (!item.is_object()) invalid();
        // 每项必须是对象。
        statement.keys.push_back({item.value("primary", false), item.at("columns").get<std::vector<std::string>>()});
        // 还原"是否主键 + 列名列表"。
    }
    if (node.contains("checks")) for (const auto& item : node.at("checks")) statement.checks.push_back(deserializeExpression(item, depth + 1));
    // 逐项还原 CHECK 条件表达式。
    if (node.contains("foreignKeys")) for (const auto& item : node.at("foreignKeys")) {
    // 逐项还原表级外键。
        if (!item.is_object()) invalid();
        // 每项必须是对象。
        statement.foreignKeys.push_back({item.at("columns").get<std::vector<std::string>>(), item.at("table").get<std::string>(), item.at("referencedColumns").get<std::vector<std::string>>()});
        // 还原本表列、父表名、父表列三部分。
    }
    if (node.contains("constraintNames")) for (const auto& item : node.at("constraintNames")) {
    // 逐项还原约束名绑定。
        if (!item.is_object() || !item.at("index").is_number_unsigned()) invalid();
        // 下标必须是非负整数，所以用无符号判断。
        statement.constraintNames.push_back({item.at("name").get<std::string>(), item.at("kind").get<std::string>(), item.at("index").get<std::size_t>()});
        // 还原名字、类别、目标下标。
    }
    if (node.contains("groupBy")) for (const auto& item : node.at("groupBy")) statement.groupBy.push_back(deserializeExpression(item, depth + 1));
    // 逐项还原 GROUP BY 分组键。
    statement.having = expression("having");
    // 还原 HAVING 条件（可选）。
    return statement;
    // 返回还原好的语句。
}
}
inline std::vector<Statement> deserializeAst(const nlohmann::json& document) {
// 反序列化一个 AST 文档（可以是带版本的文档，也可以直接是语句数组）。
    nlohmann::json nodes = document;
    // 先假设整个文档就是语句负载。
    if (document.is_object() && document.contains("schemaVersion")) {
    // 检测到带版本号的文档外壳。
        if (document.at("schemaVersion") != AST_SCHEMA_VERSION || !document.contains("statements") || !document.at("statements").is_array())
            throw MiniSqlError(ErrorCode::Storage, "Unsupported AST schema version");
        // 主版本对不上，或者缺少 statements 数组，都直接拒绝：宁可不读也不误读。
        nodes = document.at("statements");
        // 取出真正的语句负载。
    }
    if (nodes.is_object()) nodes = nlohmann::json::array({nodes});
    // 单条语句也允许直接传对象，这里统一包装成数组，简化后面的循环。
    if (!nodes.is_array() || nodes.size() > 1024) throw MiniSqlError(ErrorCode::Storage, "Invalid serialized AST document");
    // 必须是数组，且条数不超过 1024，防止超大文档造成资源耗尽。
    std::vector<Statement> statements;
    // 结果容器。
    for (const auto& node : nodes) statements.push_back(detail::readStatement(node));
    // 逐条严格还原。
    return statements;
    // 返回语句列表。
}
// 把一批 AST 序列化成带版本号的文档（schemaVersion + producerVersion），
// 这样读取方可以拒绝不认识的主版本，同时保留 statements 负载本身。
// Serialize an AST batch as a versioned document (schemaVersion + producerVersion)
// so reads can reject unknown schema versions while keeping the statements payload.
inline nlohmann::json serializeAstDocument(const std::vector<Statement>& statements) {
// 生成"带版本外壳"的 AST 文档，是落盘与跨进程传输的推荐形态。
    auto nodes = nlohmann::json::array();
    // 语句负载数组。
    for (const auto& statement : statements) nodes.push_back(serializeStatement(statement));
    // 逐条序列化语句。
    std::size_t nextId = 0;
    detail::annotateAst(nodes, nextId);
    return {{"schemaVersion", AST_SCHEMA_VERSION}, {"producerVersion", PRODUCER_VERSION}, {"statements", std::move(nodes)}};
    // 组装成"版本号 + 生产者版本 + 语句负载"三段式文档。
}
inline nlohmann::json serializeAst(const std::vector<Statement>& statements) {
// 生成不带版本外壳的裸 AST：单条语句直接给对象，多条给数组。
    auto nodes = nlohmann::json::array();
    // 语句数组。
    for (const auto& statement : statements) nodes.push_back(serializeStatement(statement));
    // 逐条序列化。
    std::size_t nextId = 0;
    detail::annotateAst(nodes, nextId);
    return nodes.size() == 1 ? nodes[0] : nodes;
    // 只有一条语句时退回成"裸对象"，方便调用方直接按单条语句使用。
}
inline nlohmann::json serializeTokens(const std::vector<Token>& tokens) {
// 把 token 流序列化成 JSON 数组，供前端展示与词法验收使用。
    auto nodes = nlohmann::json::array();
    // 结果数组。
    for (const auto& token : tokens) nodes.push_back({{"type", token.type}, {"text", token.lexeme},
        {"line", token.location.line}, {"column", token.location.column},
        {"endLine", token.endLocation.line}, {"endColumn", token.endLocation.column},
        {"byteStart", token.byteStart}, {"byteEnd", token.byteEnd}});
    return nodes;
    // 返回数组。
}
}
