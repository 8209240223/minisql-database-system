#include "minisql/execution/database.hpp"
#include "minisql/optimizer/optimizer.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/cast.hpp"
#include "minisql/common/decimal.hpp"
#include "minisql/common/float.hpp"
#include "minisql/common/filesystem.hpp"
#include "minisql/storage/bplus_tree.hpp"
#include "minisql/storage/page_bplus_tree.hpp"
#include "minisql/storage/heap.hpp"
#include "minisql/execution/external_sort.hpp"
#include "minisql/sql/serialization.hpp"
#include "minisql/sql/binding.hpp"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>

namespace minisql::execution {
namespace {
using json = nlohmann::json;
// 简写 JSON 命名空间。
thread_local Database* activeDatabase = nullptr;
// 当前线程正在执行的数据库实例。
// 用 thread_local 是为了让深层的辅助函数（如错误包装）能拿到"当前库"而不必层层传参，
// 同时保证多线程各看各的，不会互相串。
struct ActiveDatabaseScope {
// RAII 工具：进入作用域时设置当前数据库，离开时自动还原。
    explicit ActiveDatabaseScope(Database* database) : previous(activeDatabase) { activeDatabase = database; }
    // 构造时先记下旧值（支持嵌套），再把自己设为当前。
    ~ActiveDatabaseScope() { activeDatabase = previous; }
    // 析构时还原旧值，保证异常路径也不会把状态留脏。
    Database* previous;
    // 外层作用域的数据库实例（最外层是 nullptr）。
};
struct QueryResourcesScope {
    QueryResourcesScope(std::shared_ptr<QueryResourceManager>& slot,
                        std::shared_ptr<QueryResourceManager> resources)
        : slot_(slot), previous_(std::move(slot)) { slot_ = std::move(resources); }
    ~QueryResourcesScope() { slot_ = std::move(previous_); }
    std::shared_ptr<QueryResourceManager>& slot_;
    std::shared_ptr<QueryResourceManager> previous_;
};
std::string key(std::string value) {
// 把名字统一转小写，作为大小写不敏感比较的规范形式。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 逐字符转小写；入参先转无符号，避免负值传入造成未定义行为。
    return value;
    // 返回规范化结果。
}
// 把索引建造/校验阶段收集到的问题列表拼成一条诊断消息。
std::string joinProblems(const std::vector<std::string>& problems) {
    std::string message;
    for (const auto& problem : problems) {
        if (!message.empty()) message += "; ";
        message += problem;
    }
    return message;
}
// 第十七章 REQ-CORE-001：批量语句上限 10000。compile 整批进入 Parser::all()，
// 而 execute/diagnostics 自己按分号切分，因此两者共用同一常量与消息，
// 消息含 "budget exceeded" 以便 HTTP 适配层映射到 413。
constexpr std::size_t kMaxBatchStatements = 10000;
const char* const kBatchBudgetMessage = "Statement budget exceeded: batch input exceeds 10000 statements";
// 授权链路会在绑定前先判定批量上限：否则超限请求会被绑定失败掩盖成权限错误
// （HTTP 403），而不是规格书要求的资源超限（HTTP 413）。
// 用分号总数做快速排除，正常请求不产生额外词法开销；异常大输入才用
// recovery 词法器精确计数（它不对词法错误抛异常，不影响既有的 fail-closed 契约）。
void enforceBatchStatementBudget(const std::string& source) {
    if (std::count(source.begin(), source.end(), ';') <= static_cast<std::ptrdiff_t>(kMaxBatchStatements)) return;
    std::vector<MiniSqlError> ignored;
    const auto tokens = sql::tokenizeRecoverable(source, ignored);
    std::size_t statements = 0;
    for (const auto& token : tokens)
        if (token.type == "DELIMITER" && token.lexeme == ";") ++statements;
    if (statements > kMaxBatchStatements) throw MiniSqlError(ErrorCode::Execution, kBatchBudgetMessage);
}
// 缓冲池帧数：默认取构造参数，MINISQL_BUFFER_FRAMES 可覆盖（用于观察命中率与替换日志）。
// 把它做成可覆盖的，是为了在不改代码的前提下演示不同缓冲池大小对命中率的影响。
// 缓冲池帧数：默认取构造参数，MINISQL_BUFFER_FRAMES 可覆盖（用于观察命中率与替换日志）。
std::size_t resolveBufferFrames(std::size_t frames) {
// 解析最终的缓冲池帧数。
    if (const char* configured = std::getenv("MINISQL_BUFFER_FRAMES")) {
    // 环境变量存在时优先使用它。
        char* end = nullptr;
        // 记录解析停下的位置，用于确认整个字符串都被消费。
        const auto parsed = std::strtoull(configured, &end, 10);
        // 按十进制解析成无符号数。
        if (end && *end == '\0' && parsed >= 1 && parsed <= 1000000) return static_cast<std::size_t>(parsed);
        // 必须整串都是数字、且落在 1..1000000 之间；不满足就静默忽略，退回默认值。
    }
    return frames;
    // 返回构造参数给的默认帧数。
}

storage::ReplacementPolicy resolvePolicy() {
// 决定缓存淘汰策略：默认 LRU，可用 MINISQL_REPLACEMENT_POLICY 覆盖。
// 三种取值都是大小写不敏感的：LRU / FIFO / CLOCK。
    if (const char* configured = std::getenv("MINISQL_REPLACEMENT_POLICY")) {
    // 读环境变量。
        std::string value(configured);
        // 复制一份用于统一转大写。
        for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        // 全部转大写，让 lru / Lru / LRU 都能识别。
        if (value == "FIFO") return storage::ReplacementPolicy::FIFO;
        // 先进先出。
        if (value == "CLOCK") return storage::ReplacementPolicy::CLOCK;
        // 二次机会。
        if (value == "LRU") return storage::ReplacementPolicy::LRU;
        // 最近最少使用。
    }
    // 未设或值不认识时回到默认 LRU。
    return storage::ReplacementPolicy::LRU;
    // 返回默认策略。
}
std::optional<std::uint64_t> positiveEnvironmentValue(const char* name, std::uint64_t maximum) {
    const char* configured = std::getenv(name);
    if (!configured) return std::nullopt;
    char* end = nullptr;
    const auto parsed = std::strtoull(configured, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > maximum) return std::nullopt;
    return parsed;
}
json cell(const storage::Value& value) {
// 把存储层的单元格值（variant）转成 JSON。
    return std::visit([](const auto& v) -> json {
    // 用 visit 对 variant 的每种可能类型分别处理。
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) return nullptr;
        // monostate 表示 NULL，对应 JSON 的 null。
        else return json(v);
        // 其余类型直接构造 JSON（整数、浮点、字符串、布尔都能隐式转换）。
    }, value);
}
bool likeMatch(const std::string& text, const std::string& pattern) {
// LIKE 模式匹配：% 匹配任意长度子串（含空串），_ 匹配恰好一个字符，其余按字面比较。
// 用迭代回溯实现，避免递归带来的栈开销：记录最近一次 % 的位置与当时匹配到的文本下标。
    std::size_t ti = 0, pi = 0;
// ti 是文本游标，pi 是模式游标。
    std::size_t starPattern = std::string::npos, starText = 0;
// starPattern 记录最近一个 % 在模式中的位置，starText 记录它当时对应的文本位置。
    while (ti < text.size()) {
// 只要文本还没走完就继续匹配。
        if (pi < pattern.size() && (pattern[pi] == '_' || pattern[pi] == text[ti])) {
// 单字符匹配：_ 吃掉任意一个字符，普通字符要求逐字相等。
            ++ti; ++pi;
// 两个游标一起前进。
        } else if (pi < pattern.size() && pattern[pi] == '%') {
// 遇到 %：先假设它匹配空串，记住这个可以回溯的位置。
            starPattern = pi; starText = ti; ++pi;
// 记下 % 的位置与当前文本位置，模式游标前进一格。
        } else if (starPattern != std::string::npos) {
// 上面两条都不成立，但之前见过 %：让它多吞一个字符再重试。
            pi = starPattern + 1; ++starText; ti = starText;
// 模式游标回到 % 之后，文本多前进一个字符。
        } else {
// 没有可回溯的 %，说明匹配失败。
            return false;
        }
    }
// 文本已经走完。
    while (pi < pattern.size() && pattern[pi] == '%') ++pi;
// 模式尾部剩余的 % 可以匹配空串，全部跳过。
    return pi == pattern.size();
// 模式也走完才算整体匹配成功。
}
bool accepted(const json& value) { return value.is_boolean() && value.get<bool>(); }
// 判断谓词求值结果是否"成立"：只有明确的 true 才算通过（NULL 与 false 都不通过）。
bool hashJoinKeys(const json& predicate, std::size_t leftSize, std::size_t& leftKey, std::size_t& rightKey) {
// 判断连接谓词是不是"左表某列 = 右表某列"，是就给出两侧的键。
    if (!predicate.is_object() || predicate.value("kind", "") != "Binary" || predicate.value("operator", "") != "=") return false;
    // 必须是等值比较。
    const auto& left = predicate.at("left");
    // 左操作数。
    const auto& right = predicate.at("right");
    // 右操作数。
    if (!left.is_object() || !right.is_object() || left.value("kind", "") != "Identifier" || right.value("kind", "") != "Identifier") return false;
    // 两侧都必须是裸列引用；表达式参与等值比较不能直接用哈希连接。
    const auto a = left.at("columnId").get<std::size_t>();
    // 左侧列编号。
    const auto b = right.at("columnId").get<std::size_t>();
    // 右侧列编号。
    if (a < leftSize && b >= leftSize) { leftKey = a; rightKey = b - leftSize; return true; }
    // 左小右大：右侧编号要减去左表列数，换算成右表内的编号。
    if (b < leftSize && a >= leftSize) { leftKey = b; rightKey = a - leftSize; return true; }
    // 反过来同理，这样等号两边写反也能识别。
    return false;
    // 两侧都属于同一张表，不是跨表等值条件。
}
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Execution, message); }
// 统一的执行错误出口：本文件里所有运行期问题都按 Execution 错误码抛出。
json cell(const json& value) { return value; }
// 重载：值本身已经是 JSON 时原样返回，让下面的模板代码对两种行类型都能工作。
template <typename Row>
// 模板函数：行类型既可能是存储层的 Row，也可能是 JSON 数组。
json rowJson(const Row& row) {
// 把一行数据转成 JSON 数组。
    if constexpr (std::is_same_v<std::decay_t<Row>, json>) return row;
    // 已经是 JSON 就直接返回，不用逐元素转换。
    json result = json::array();
    // 结果数组。
    for (const auto& value : row) result.push_back(cell(value));
    // 逐列转换后追加。
    return result;
    // 返回整行。
}
ExactDecimal decimalValue(const json& value, const std::string& type) {
// 把一个 JSON 值按给定类型还原成精确十进制数。
    const auto decimal = decimalType(type);
    // 看类型是不是 decimal(p,s)。
    return decimal ? ExactDecimal::parse(value.get<std::string>(), decimal->precision, decimal->scale) : ExactDecimal::fromInteger(value.get<std::int64_t>());
    // 是定点数就按它自己的精度标度解析文本；否则按 64 位整数取值。
    // 两条路都得到同一个 ExactDecimal，后续可以统一参与精确运算。
}
// X09 3.5: 相关子查询 by-value 参数绑定执行。序列化外层列绑定值为 SQL 字面量
// X09 3.5：相关子查询按值绑定参数来执行。
// 文本（与解析器产出的 Literal 一致），随后绑定进已缓存的结构化 AST，取代原
// 做法是把外层列的当前值序列化成 SQL 字面量文本（与解析器产出的 Literal 一致），
// 先“文本重解析 + 字面量改写”的路径。
// 再绑定进已经缓存好的结构化语法树——这样就不必每行都重新做一次文本解析与改写。
// X09 3.5: 相关子查询 by-value 参数绑定执行。序列化外层列绑定值为 SQL 字面量
// 文本（与解析器产出的 Literal 一致），随后绑定进已缓存的结构化 AST，取代原
// 先“文本重解析 + 字面量改写”的路径。
using OuterBinding = std::unordered_map<std::string, std::pair<std::size_t, std::string>>;
// 外层列绑定的形状：列名 → (列编号, 类型)。
using OuterValues = std::unordered_map<std::string, std::pair<json, std::string>>;
thread_local std::vector<OuterValues> activeOuterValues;
struct ActiveOuterScope {
    explicit ActiveOuterScope(OuterValues values) { activeOuterValues.push_back(std::move(values)); }
    ~ActiveOuterScope() { activeOuterValues.pop_back(); }
};
std::string parameterLiteral(const json& value, const std::string& type) {
// 把一个值按它的类型格式化成 SQL 字面量文本。
    if (value.is_null()) return "NULL";
    // NULL 直接写 NULL。
    if (type == "bool") return value.get<bool>() ? "TRUE" : "FALSE";
    // 布尔按 TRUE/FALSE 输出。
    if (type == "float") return formatFiniteFloat(value.get<double>());
    // 浮点按有限值格式化（NaN/Infinity 不允许进入文本）。
    if (type == "int" || type == "bigint") return std::to_string(value.get<std::int64_t>());
    // 整数直接转十进制文本。
    if (decimalType(type)) return value.is_string() ? value.get<std::string>() : std::to_string(value.get<std::int64_t>());
    // 定点数优先用已有的字符串形式（保持定标度），否则退化成整数文本。
    const auto text = value.get<std::string>();
    // 剩下的都是字符串型（日期、各种字符串类型）。
    std::string quoted;
    // 转义后的文本。
    for (const char ch : text) { quoted += ch; if (ch == '\'') quoted += '\''; }
    // 逐个字符复制，遇到单引号再补一个——这是 SQL 标准的字符串转义方式，
    // 能防止值里的引号提前结束字面量（也就是注入）。
    if (type == "date") return "DATE'" + quoted + "'";
    // 日期要带 DATE 前缀，否则解析器认不出是日期字面量。
    return "'" + quoted + "'";
    // 普通字符串加单引号。
}
std::shared_ptr<sql::Expr> bindOuter(const std::shared_ptr<sql::Expr>& expression, const OuterBinding& outer, const json& row, std::size_t depth = 0) {
// 把表达式里引用的外层列替换成"该行对应的字面量"。
// 这是相关子查询按值绑定的核心：同一个缓存的 AST，配合不同的行值反复使用。
    if (!expression) return nullptr;
    // 空节点直接返回。
    if (depth > 256) fail("correlated subquery expression depth exceeded");
    // 深度保护，避免畸形表达式把递归栈打爆。
    if (expression->kind == "Identifier") {
    // 列引用才有可能需要替换。
        const auto found = outer.find(key(expression->value));
        // 在"外层列名 → (列编号, 类型)"映射里查它（大小写不敏感）。
        if (found != outer.end()) {
        // 命中说明这个列引用指向外层。
            const auto columnId = found->second.first;
            // 取出它在外层行里的列编号。
            if (columnId >= row.size()) fail("Correlated subquery outer column outside row");
            // 编号越界说明编译期与运行期的行结构对不上，属于内部不变量破坏。
            auto literal = std::make_shared<sql::Expr>();
            // 造一个字面量节点。
            literal->kind = "Literal";
            // 种类是字面量。
            literal->value = parameterLiteral(row.at(columnId), found->second.second);
            // 值取当前行对应列，并按列类型格式化；这样同一个 AST 就能用不同行的值填充。
            literal->location = expression->location;
            return literal;
        }
        for (auto frame = activeOuterValues.rbegin(); frame != activeOuterValues.rend(); ++frame) {
            const auto inherited = frame->find(key(expression->value));
            if (inherited == frame->end()) continue;
            auto literal = std::make_shared<sql::Expr>();
            literal->kind = "Literal";
            literal->value = parameterLiteral(inherited->second.first, inherited->second.second);
            literal->location = expression->location;
            // 位置沿用原列引用，报错仍指向用户写的那一处。
            return literal;
            // 返回替换后的字面量。
        }
        // 命中判断结束。
    }
    // 列引用分支结束。
    auto cloned = std::make_shared<sql::Expr>();
    // 其它节点做浅拷贝。
    cloned->kind = expression->kind;
    // 种类不变。
    cloned->value = expression->value;
    // 值不变。
    cloned->location = expression->location;
    // 位置不变。
    cloned->subquerySql = expression->subquerySql;
    // 子查询原文也带过去（若有）。
    if (expression->left) cloned->left = bindOuter(expression->left, outer, row, depth + 1);
    // 递归处理左子树（注意这里传的是新拷贝，不改动缓存里的原 AST）。
    if (expression->right) cloned->right = bindOuter(expression->right, outer, row, depth + 1);
    // 递归处理右子树。
    return cloned;
    // 返回新节点。
}
sql::Statement bindOuterStatement(const sql::Statement& statement, const OuterBinding& outer, const json& row) {
// 对整条语句做外层列绑定：把语句里所有表达式位置都过一遍 bindOuter。
    sql::Statement out = statement;
    // 先整体拷贝一份语句（未列出的字段沿用原值，例如表名、列定义）。
    out.where = bindOuter(statement.where, outer, row);
    // WHERE 条件。
    for (auto& item : out.selectItems) item.expression = bindOuter(item.expression, outer, row);
    // 每个投影表达式。
    for (auto& item : out.orderBy) item.expression = bindOuter(item.expression, outer, row);
    // 每个排序键。
    for (auto& assignment : out.assignments) assignment.expression = bindOuter(assignment.expression, outer, row);
    // UPDATE 赋值。
    for (auto& check : out.checks) check = bindOuter(check, outer, row);
    // CHECK 约束。
    for (auto& value : out.valueExpressions) value = bindOuter(value, outer, row);
    // INSERT 的单行表达式值。
    for (auto& valueRow : out.valueRows) for (auto& value : valueRow) value = bindOuter(value, outer, row);
    // INSERT 的多行表达式值（逐行逐列）。
    for (auto& column : out.groupBy) column = bindOuter(column, outer, row);
    // 分组键。
    out.having = bindOuter(statement.having, outer, row);
    // HAVING 条件。
    for (auto& join : out.joins) join.on = bindOuter(join.on, outer, row);
    // 每个连接的 ON 条件。
    if (statement.fromSubquery)
    // FROM 是派生表时。
        out.fromSubquery = std::make_shared<sql::Statement>(bindOuterStatement(*statement.fromSubquery, outer, row));
        // 递归处理内层语句——外层引用可能出现在任意一层。
    return out;
    // 返回绑定后的语句。
}
// X09 3.4: 收集相关子查询 AST 中实际引用到的外层列 columnId（去重、升序），
// 用于按绑定参数分组建缓存键。遍历字段与 bindOuterStatement 对齐。
// X09 3.4: 收集相关子查询 AST 中实际引用到的外层列 columnId（去重、升序），
// 用于按绑定参数分组建缓存键。遍历字段与 bindOuterStatement 对齐。
void collectOuterReferences(const std::shared_ptr<sql::Expr>& expression, const OuterBinding& outer, std::set<std::size_t>& ids) {
// 收集表达式里实际引用到的外层列编号（去重、升序）。
// 用途：把"引用了哪些外层列"作为缓存键的一部分，同一组绑定值只物化一次子查询。
    if (!expression) return;
    // 空节点返回。
    if (expression->kind == "Identifier") {
    // 只看列引用。
        const auto found = outer.find(key(expression->value));
        // 在映射里查它是不是外层列。
        if (found != outer.end()) ids.insert(found->second.first);
        // 是就把列编号记进集合（集合天然去重且有序）。
    }
    // 列引用分支结束。
    if (expression->left) collectOuterReferences(expression->left, outer, ids);
    // 递归左子树。
    if (expression->right) collectOuterReferences(expression->right, outer, ids);
    // 递归右子树。
}
void collectStatementOuterReferences(const sql::Statement& statement, const OuterBinding& outer, std::set<std::size_t>& ids) {
// 语句级版本：遍历的位置与 bindOuterStatement 严格对齐，
// 这样"收集到的引用"与"实际会替换的位置"永远是一致的。
    collectOuterReferences(statement.where, outer, ids);
    // WHERE。
    for (const auto& item : statement.selectItems) collectOuterReferences(item.expression, outer, ids);
    // 投影。
    for (const auto& item : statement.orderBy) collectOuterReferences(item.expression, outer, ids);
    // 排序键。
    for (const auto& assignment : statement.assignments) collectOuterReferences(assignment.expression, outer, ids);
    // 赋值表达式。
    for (const auto& check : statement.checks) collectOuterReferences(check, outer, ids);
    // CHECK 约束。
    for (const auto& value : statement.valueExpressions) collectOuterReferences(value, outer, ids);
    // INSERT 表达式值。
    for (const auto& valueRow : statement.valueRows) for (const auto& value : valueRow) collectOuterReferences(value, outer, ids);
    // INSERT 多行值。
    for (const auto& column : statement.groupBy) collectOuterReferences(column, outer, ids);
    // 分组键。
    collectOuterReferences(statement.having, outer, ids);
    // HAVING。
    for (const auto& join : statement.joins) collectOuterReferences(join.on, outer, ids);
    // 连接的 ON 条件。
    if (statement.fromSubquery) collectStatementOuterReferences(*statement.fromSubquery, outer, ids);
    // 派生表递归处理。
}
std::string storedDecimal(const json& value, const storage::ColumnSchema& column, SourceLocation location) {
// 把值按列的精度标度规范化成存储用的定点数字符串。
    try {
    // 解析可能因为超精度而失败。
        const auto text = value.is_number_integer() ? std::to_string(value.get<std::int64_t>()) : value.get<std::string>();
        // 整数先转文本；非整数项按字符串取（定点数在 JSON 里就是字符串）。
        return ExactDecimal::parse(text, column.precision, column.scale).format();
        // 按列声明的精度标度解析后重新格式化，保证存进去的表示是统一规格。
    } catch (const MiniSqlError& error) { throw MiniSqlError(error.code(), error.what(), location); }
    // 失败时保留错误码与信息，只把位置换成调用方给的（这样报错指向用户写的那一处）。
}
storage::Value indexValue(const json& value, const std::string& type) {
// 把 JSON 值转成可以放进索引键的存储层值。
    if (value.is_null()) return std::monostate{};
    // NULL 用 monostate 表示。
    if (type == "int") return value.get<std::int32_t>();
    // 32 位整数。
    if (type == "bigint") return value.get<std::int64_t>();
    // 64 位整数。
    if (type == "float") return requireFiniteFloat(value.get<double>());
    // 浮点要确认是有限值（NaN/Infinity 不能进索引）。
    if (type == "bool") return value.get<bool>();
    // 布尔。
    return value.get<std::string>();
    // 其余（字符串、日期、定点数）在存储层统一按字符串保存。
}
double storedFloat(const json& value, SourceLocation location) {
// 把值还原成浮点数，并拒绝非有限值。
    if (value.is_number_float()) return requireFiniteFloat(value.get<double>(), location);
    // 本来就是浮点。
    if (value.is_number_integer()) return requireFiniteFloat(static_cast<double>(value.get<std::int64_t>()), location);
    // 整数转成浮点（一定有限）。
    if (value.is_string()) return parseFiniteFloat(value.get_ref<const std::string&>(), ErrorCode::Execution, location);
    // 字符串形式的浮点按文本解析，同样要求有限值。
    throw MiniSqlError(ErrorCode::Execution, "FLOAT column requires a numeric value", location);
    // 其它类型一律拒绝。
}
template <typename Row>
// 表达式求值模板：行既可能是存储层 Row，也可能是 JSON 数组。
json evaluate(const json& expression, const Row& row) {
// 求值一个计划表达式；返回 JSON 形式的值。
    const auto kind = expression.at("kind").get<std::string>();
    // 节点种类。
    if (kind == "Literal") return expression.at("value");
    // 字面量直接取其值。
    if (kind == "Identifier") {
    // 列引用：从当前行取对应列。
        const auto index = expression.at("columnId").get<std::size_t>();
        // 列编号。
        if (index >= row.size()) fail("Plan column outside row");
        // 越界说明计划与行结构不匹配，属于内部不变量破坏。
        return cell(row[index]);
        // 转成 JSON 返回。
    }
    // 列引用分支结束。
    if (kind == "CorrelatedExists") {
    // 相关 EXISTS：交给数据库实例去执行（它持有缓存与目录）。
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        // 没有活动数据库上下文说明调用路径不对，明确报错而不是空指针崩溃。
        return !activeDatabase->runCorrelatedSubquery(expression, rowJson(row)).empty();
        // 子查询有结果就算成立（EXISTS 只关心有没有行）。
    }
    // CorrelatedExists 分支结束。
    if (kind == "CorrelatedScalarSubquery") {
    // 相关标量子查询。
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        // 同样要求有活动数据库上下文。
        const auto rows = activeDatabase->runCorrelatedSubquery(expression, rowJson(row));
        // 执行子查询。
        if (rows.empty()) return nullptr;
        // 没有匹配行时，标量子查询的结果是 NULL。
        if (rows.size() != 1 || !rows.front().is_array() || rows.front().size() != 1)
        // 标量子查询必须最多返回一行一列。
            fail("Correlated scalar subquery returned more than one row or column");
            // 违反这个约定属于运行期错误（SQL 语义如此）。
        return rows.front().front();
        // 返回那唯一的值。
    }
    // 标量子查询分支结束。
    if (kind == "CorrelatedInSubquery") {
    // 相关 IN 子查询：需要按"左值 IN (子查询结果)"的三值逻辑求值。
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        // 上下文检查。
        const auto left = evaluate(expression.at("left"), row);
        // 先算出左边待比较的值。
        if (left.is_null()) return nullptr;
        // 左边是 NULL 时，整个 IN 的结果就是 UNKNOWN（NULL），不必再跑子查询。
        const auto rows = activeDatabase->runCorrelatedSubquery(expression, rowJson(row));
        // 执行子查询。
        bool hasNull = false;
        // 记录结果集里是否出现过 NULL——这决定"没匹配上"时是 UNKNOWN 还是 FALSE。
        for (const auto& candidate : rows) {
        // 逐行比较。
            if (!candidate.is_array() || candidate.size() != 1) fail("Correlated IN subquery must return one column");
            // IN 的子查询只能有一列。
            if (candidate.front().is_null()) hasNull = true;
            // 遇到 NULL 只记标记，不能直接判定结果（这正是三值逻辑的关键）。
            else if (candidate.front() == left) return true;
            // 命中就直接返回真。
        }
        return hasNull ? json(nullptr) : json(false);
        // 没命中：结果集里有 NULL 就返回 UNKNOWN，否则确定返回假。
    }
    // IN 子查询分支结束。
    // 未物化的子查询节点没有 left/operator 字段，直接取键会抛出 json 异常。
    // 这里转成正式诊断，避免把内部异常当成 InternalError 泄露给调用方。
    if (kind == "ScalarSubquery" || kind == "Exists" || kind == "InSubquery") {
        const std::string detail = "Subquery was not materialized before evaluation: " + kind;
        fail(detail.c_str());
    }
    const json left = evaluate(expression.at("left"), row);
    // 其余节点都至少有一个左操作数，这里统一先求值。
    const SourceLocation location{expression.value("line", std::size_t(0)), expression.value("column", std::size_t(0))};
    // 取出源位置，后面报错与转换都要用。
    if (kind == "Cast") {
    // 类型转换。
        if (decimalType(expression.at("type").get<std::string>())) return castValue(left, expression.at("type").get<std::string>(), location);
        // 目标是定点数：走统一的转换函数。
        if (!left.is_null() && decimalType(expression.at("left").at("type").get<std::string>())) {
        // 源是定点数且值非空时，需要先把它从"定标度字符串"还原出来。
            if (stringType(expression.at("type").get<std::string>())) return castValue(left,expression.at("type").get<std::string>(),location);
            // 目标又是字符串类型，那就直接用原来的文本（保持定标度可读）。
            return castValue(ExactDecimal::parse(left.get<std::string>(), 38, 0, true).format(), expression.at("type").get<std::string>(), location);
            // 否则按最宽精度重新格式化后再转换——先保住数值，再由目标类型决定最终形态。
        }
        // 定点数源处理结束。
        return castValue(left, expression.at("type").get<std::string>(), location);
        // 其它情况直接转换。
    }
    // Cast 分支结束。
    const auto op = expression.at("operator").get<std::string>();
    // 取出运算符。
    if (op == "IS NULL") return left.is_null();
    // IS NULL 直接看左值是否为空。
    if (op == "IS NOT NULL") return !left.is_null();
    // 取反。
    if (kind == "Unary" && (op == "+" || op == "-") && decimalType(expression.at("type").get<std::string>())) {
    // 定点数的一元正负号。
        if (left.is_null()) return nullptr;
        // NULL 取负还是 NULL。
        const auto decimal = decimalValue(left, expression.at("type").get<std::string>());
        // 还原成精确十进制。
        return (op == "-" ? decimal.negated() : decimal).format();
        // 取负或原样，再格式化回定标度字符串。
    }
    // 定点数一元分支结束。
    if (kind == "Unary" && (op == "+" || op == "-") && expression.at("type") == "float") {
    // 浮点的一元正负号。
        if (left.is_null()) return nullptr;
        // NULL 传播。
        const auto number = requireFiniteFloat(left.get<double>(), location);
        // 取有限值。
        return requireFiniteFloat(op == "-" ? -number : number, location);
        // 取负后仍要确认是有限值（避免 -0.0 之外的边界问题）。
    }
    // 浮点一元分支结束。
    if (kind == "Unary" && (op == "+" || op == "-"))
    // 整数的一元正负号。
        return left.is_null() ? json(nullptr) : expression.at("type") == "bigint" ? json(arithmetic64(op, 0, left.get<std::int64_t>(), location)) : json(arithmetic(op, 0, left.get<std::int32_t>(), location));
        // 统一按 "0 op 值" 的形式调用运算函数，这样取负的溢出检查与加减法共用同一套逻辑；
        // bigint 走 64 位版本，其余走 32 位版本。
    if (op == "NOT") return left.is_null() ? json(nullptr) : json(!left.get<bool>());
    // 逻辑非：三值逻辑下 NOT NULL 仍是 NULL。
    if (op == "AND" && left == false) return false;
    // 短路优化：AND 左边为假，整个表达式必假，右侧不求值。
    if (op == "OR" && left == true) return true;
    // 同理 OR 左边为真直接返回真。
    const json right = evaluate(expression.at("right"), row);
    // 走到这里说明必须求右侧了。
    if (op == "LIKE" || op == "NOT LIKE") {
    // LIKE 模式匹配：% 匹配任意长度子串（含空串），_ 匹配单个字符，其余按字面比较。
    // 三值逻辑下任一侧为 NULL 时结果也是 NULL（与 SQL 标准一致）。
        if (left.is_null() || right.is_null()) return nullptr;
    // 任一操作数为空直接返回 NULL。
        if (!left.is_string() || !right.is_string()) fail("LIKE requires string operands");
    // 两侧都必须是字符串；混合类型属于用法错误。
        const bool matched = likeMatch(left.get<std::string>(), right.get<std::string>());
    // 按通配符规则逐字符比对。
        return op == "NOT LIKE" ? json(!matched) : json(matched);
    // NOT LIKE 取反。
    }
    if (op == "AND" || op == "OR") {
    // 逻辑与/或的三值真值表。
        if (op == "AND" && right == false) return false;
        // 右侧为假，AND 必假。
        if (op == "OR" && right == true) return true;
        // 右侧为真，OR 必真。
        if (left.is_null() || right.is_null()) return nullptr;
        // 两边都不是决定性的值，且任一为 NULL，结果就是 NULL。
        return right.get<bool>();
        // 否则结果等于右侧的布尔值（左侧在短路检查里已经排除了假/真）。
    }
    // 逻辑分支结束。
    if (left.is_null() || right.is_null()) return nullptr;
    // 到这里两边都非 NULL；任一边为 NULL 时结果按三值逻辑为 NULL，所以先挡掉。
    const bool leftFloat = expression.at("left").at("type") == "float";
    // 左侧是不是浮点类型。
    const bool rightFloat = expression.at("right").at("type") == "float";
    // 右侧是不是浮点类型。
    if (leftFloat || rightFloat) {
    // 任一侧是浮点就整条走浮点路径——混合类型运算以浮点为准。
        const auto a = requireFiniteFloat(left.get<double>(), location);
        // 取左侧有限浮点值。
        const auto b = requireFiniteFloat(right.get<double>(), location);
        // 取右侧有限浮点值。
        if (isArithmetic(op)) {
        // 算术运算。
            if (op == "+") return requireFiniteFloat(a + b, location);
            // 加法；结果仍要检查是否为有限值（可能溢出成无穷）。
            if (op == "-") return requireFiniteFloat(a - b, location);
            // 减法。
            if (op == "*") return requireFiniteFloat(a * b, location);
            // 乘法。
            if (op == "/") return requireFiniteFloat(a / b, location);
            // 除法（除以 0 会产生无穷，这里会被拦下）。
        }
        // 算术分支结束。
        if (op == "=") return a == b;
        // 浮点相等比较。
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        fail("Unsupported FLOAT operator");
        // 其它运算符在浮点路径下没有实现，明确报错而不是给错结果。
    }
    // 浮点分支结束。
    if (decimalType(expression.at("left").at("type").get<std::string>()) || decimalType(expression.at("right").at("type").get<std::string>())) {
    // 任一侧是定点数就走精确十进制路径（不能用 double，会丢精度）。
        const auto a = decimalValue(left, expression.at("left").at("type").get<std::string>());
        // 还原左侧的精确十进制值。
        const auto b = decimalValue(right, expression.at("right").at("type").get<std::string>());
        // 还原右侧。
        if (isArithmetic(op)) return a.arithmetic(op, b, location).format();
        // 算术运算用精确运算实现，结果再格式化成定标度字符串。
        const auto order = a.compare(b);
        // 比较先算出大小关系（-1/0/1），再按运算符翻译成布尔值。
        if (op == "=") return order == 0;
        // 相等。
        if (op == "!=") return order != 0;
        if (op == "<") return order < 0;
        if (op == "<=") return order <= 0;
        if (op == ">") return order > 0;
        if (op == ">=") return order >= 0;
        fail("Unsupported DECIMAL operator");
        // 其它运算符在定点数路径下没有实现。
    }
    // 定点数分支结束。
    if (isArithmetic(op)) return expression.at("type") == "bigint" ? json(arithmetic64(op, left.get<std::int64_t>(), right.get<std::int64_t>(), location)) : json(arithmetic(op, left.get<std::int32_t>(), right.get<std::int32_t>(), location));
    // 剩下的都是整数：bigint 走 64 位运算，其余走 32 位。
    // 两个函数都会做溢出检查，溢出即按执行错误抛出。
    if (op == "=") return left == right;
    // 整数相等。
    if (op == "!=") return left != right;
    if (op == "<") return left < right;
    if (op == "<=") return left <= right;
    if (op == ">") return left > right;
    if (op == ">=") return left >= right;
    fail("Unknown expression operator");
    // 走到这里说明计划里出现了求值器不认识的运算符，属于内部错误。
}
storage::RowSchema rowSchema(const sql::Statement& definition) {
// 把建表语句里的列类型翻译成存储层的行结构（列类型列表）。
    storage::RowSchema result;
    // 结果列表，顺序与建表语句里的列一一对应。
    for (const auto& c : definition.columns) {
    // 逐列处理。
        if (key(c.type) == "int") result.push_back(storage::ColumnType::Int);
        // 32 位整数。
        else if (key(c.type) == "bigint") result.push_back(storage::ColumnType::Bigint);
        // 64 位整数。
        else if (key(c.type) == "varchar") result.push_back(storage::ColumnType::Varchar);
        // 不定长字符串。
        else if (const auto limit = varcharLength(key(c.type))) result.push_back(storage::ColumnSchema::varchar(*limit));
        // 带长度的 varchar(n)：把长度一起带进列结构，存储层据此做上限校验。
        else if (key(c.type) == "bool") result.push_back(storage::ColumnType::Bool);
        // 布尔。
        else if (key(c.type) == "date") result.push_back(storage::ColumnType::Date);
        // 日期。
        else if (key(c.type) == "float") result.push_back(storage::ColumnType::Float);
        // 浮点。
        else if (const auto decimal = decimalType(key(c.type))) result.emplace_back(decimal->precision, decimal->scale);
        // 定点数：把精度与标度都带过去（存的是定标度表示，不能丢这两个数）。
        else fail("Unsupported persisted type");
        // 其它类型不该走到这里——语义层已经拦过，所以按执行错误抛出（内部不变量破坏）。
    }
    return result;
    // 返回行结构。
}
}
struct Database::RuntimeIndex {
// 运行期索引：一份描述（名字、表、列、唯一性、用哪套引擎）加两套实现之一。
    RuntimeIndex(std::string indexName, std::string tableName, std::vector<std::size_t> columnIndices, bool isUnique, bool usePage)
        // 参数依次是索引名、表名、索引列的下标、是否唯一、是否用页级引擎。
        : name(std::move(indexName)), table(std::move(tableName)), columns(std::move(columnIndices)), unique(isUnique),
          // 前三项直接接管入参（move 避免复制），并记下唯一性。
          pageFile(usePage), tree(64, isUnique) {}
          // 记下用哪套引擎；内存版 B+ 树按 64 阶构造。
    std::string name;
    // 索引名。
    std::string table;
    // 所属表名。
    std::vector<std::size_t> columns;
    // 索引列在表内的下标（复合索引按顺序）。
    bool unique;
    // 是否唯一索引。
    bool pageFile;
    // 用页级引擎（true）还是内存引擎（false）。
    storage::BPlusTree tree;                                        // memory 引擎
    // 内存版 B+ 树实例（内存引擎时使用）。
    std::unique_ptr<storage::PageBPlusTree> pageTree{nullptr};      // page-file 引擎
    // 页级 B+ 树实例（页级引擎时使用）；用智能指针是因为它需要延迟构造。
    storage::IndexKey keyFor(const storage::Row& row) const {
        storage::IndexKey result;
        for (const auto column : columns) result.values.push_back(row.at(column));
        return result;
    }
    static bool indexable(const storage::IndexKey& key) {
        return std::none_of(key.values.begin(), key.values.end(), [](const storage::Value& value) {
            return std::holds_alternative<std::monostate>(value);
        });
    }
    bool insert(const storage::IndexKey& key, storage::RowRef row) {
        if (!indexable(key)) return true;
        return pageFile ? pageTree->insert(key, row) : tree.insert(key, row);
    }
    bool erase(const storage::IndexKey& key, storage::RowRef row) {
        if (!indexable(key)) return true;
        return pageFile ? pageTree->erase(key, row) : tree.erase(key, row);
    }
    std::vector<storage::RowRef> search(const storage::IndexKey& key) const {
    // 按精确键查找。
        return pageFile ? pageTree->search(key) : tree.search(key);
        // 按引擎分派到对应的实现。
    }
    std::vector<storage::RowRef> range(const std::optional<storage::IndexKey>& lower, bool lowerInclusive,
    // 范围查找：下界（可为空）、下界是否包含、
                                       const std::optional<storage::IndexKey>& upper, bool upperInclusive) const {
                                       // 上界与上界是否包含。
        return pageFile ? pageTree->range(lower, lowerInclusive, upper, upperInclusive)
                        // 页级引擎的范围查找，
                        : tree.range(lower, lowerInclusive, upper, upperInclusive);
                        // 或内存引擎的。
    }
    std::size_t height() const { return pageFile ? pageTree->height() : tree.height(); }
    // 返回索引树高（诊断与性能曲线用）。
    std::size_t size() const { return pageFile ? pageTree->size() : tree.size(); }
    // 返回索引条目数。
    bool validate() const { return pageFile ? pageTree->validate() : tree.validate(); }
    // 做一次结构自检（页类型、键数、兄弟指针等），供索引检查接口使用。
};
Database::Database(const std::filesystem::path& path, std::size_t frames, storage::PageFile::CommitObserver observer)
// 构造数据库：按"页文件 → 缓冲池 → 堆存储 → 持久化目录"的顺序把各层串起来。
    : file_(std::make_shared<storage::PageFile>(path, std::move(observer))), buffer_(file_, resolveBufferFrames(frames), resolvePolicy()),
      // 先建页文件（接管提交观察者），再用它建缓冲池并指定帧数与 LRU 策略。
      heap_(file_, buffer_), catalog_(heap_) {
      // 堆存储依赖页文件与缓冲池；目录又依赖堆存储。这个顺序不能颠倒。
    lastCheckpointAt_ = std::chrono::steady_clock::now();
    // 记下构造时刻作为"上次检查点时间"（单调时钟，用于算间隔）。
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 同时记一份墙上时钟的毫秒时间戳，
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 因为对外展示需要可读时间，而内部比较需要单调时钟。
    if (const char* engine = std::getenv("MINISQL_INDEX_ENGINE"); engine && std::string(engine) == "memory") pageFileIndexes_ = false;
    // 显式指定 memory 时关掉页级索引引擎（用于对比两套实现的性能与行为）。
    if (const char* configured = std::getenv("MINISQL_SORT_MEMORY_ROWS")) {
    // 排序算子的内存行数上限。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        // 解析成无符号数。
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) sortMemoryRows_ = static_cast<std::size_t>(parsed);
        // 必须整串是数字且落在 1..1000000，否则忽略（保持默认值）。
    }
    // 排序上限处理结束。
    if (const char* configured = std::getenv("MINISQL_AGGREGATE_MEMORY_ROWS")) {
    // 聚合算子的内存行数上限。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) aggregateMemoryRows_ = static_cast<std::size_t>(parsed);
        // 同样做范围校验。
    }
    // 聚合上限处理结束。
    if (const auto configured = positiveEnvironmentValue("MINISQL_DISTINCT_MEMORY_ROWS", 1000000))
        distinctMemoryRows_ = static_cast<std::size_t>(*configured);
    else distinctMemoryRows_ = sortMemoryRows_;
    if (const auto configured = positiveEnvironmentValue("MINISQL_JOIN_MEMORY_ROWS", 1000000))
        joinMemoryRows_ = static_cast<std::size_t>(*configured);
    if (const auto configured = positiveEnvironmentValue("MINISQL_QUERY_MEMORY_BYTES", 16ull * 1024ull * 1024ull * 1024ull))
        queryMemoryBytes_ = static_cast<std::size_t>(*configured);
    if (const auto configured = positiveEnvironmentValue("MINISQL_TEMP_DISK_BYTES", 1024ull * 1024ull * 1024ull * 1024ull))
        tempDiskBytes_ = *configured;
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_WRITES")) {
    // 自动检查点的"写语句数"阈值。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) autoCheckpointWrites_ = static_cast<std::size_t>(parsed);
        // 范围校验。
    }
    // 写语句阈值处理结束。
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_WAL_BYTES")) {
    // 自动检查点的 WAL 字节阈值。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1024ull * 1024ull * 1024ull) autoCheckpointWalBytes_ = parsed;
        // 上限给到 1 GiB——日志再大也该先做检查点。
    }
    // WAL 阈值处理结束。
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES")) {
    // 自动检查点的脏页数阈值。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) autoCheckpointDirtyPages_ = static_cast<std::size_t>(parsed);
        // 范围校验。
    }
    // 脏页数处理结束。
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO")) {
    // 脏页比例阈值。
        char* end = nullptr;const auto parsed = std::strtod(configured, &end);
        // 这次用浮点解析。
        if (end && *end == '\0' && std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) autoCheckpointDirtyRatio_ = parsed;
        // 必须是有限值且落在 (0,1]，避免 NaN 或超过 100% 这种无意义的配置。
    }
    // 脏页比例处理结束。
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_INTERVAL_MS")) {
    // 自动检查点的时间间隔。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 365ull * 24ull * 60ull * 60ull * 1000ull) autoCheckpointIntervalMs_ = parsed;
        // 上限给到一年——再长就等于关掉了。
    }
    // 时间间隔处理结束。
    if (const char* configured = std::getenv("MINISQL_MAX_RESULT_ROWS")) {
    // 单条语句的最大返回行数。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 10000000) maxResultRows_ = static_cast<std::size_t>(parsed);
        // 上限一千万行。
    }
    // 结果行数上限处理结束。
    if (const char* configured = std::getenv("MINISQL_RESULT_CACHE")) {
    // 结果缓存开关：显式写 0 关闭，其余值保持默认开启。
        resultCacheEnabled_ = !(configured[0] == '0' && configured[1] == '\0');
        // 只有单个字符 '0' 才算关闭；空串等其他值都视为开启。
    }
    // 结果缓存开关处理结束。
    if (const char* configured = std::getenv("MINISQL_RESULT_CACHE_MAX_ROWS")) {
    // 单条结果可缓存的最大行数。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) resultCacheMaxRows_ = static_cast<std::size_t>(parsed);
        // 上限一百万行，避免误配导致内存爆掉。
    }
    // 结果缓存行数上限处理结束。
    sortTempDirectory_ = std::getenv("MINISQL_TEMP_DIR") ? std::filesystem::path(std::getenv("MINISQL_TEMP_DIR")) : path.parent_path() / ".minisql-sort";
    // 排序溢出目录：可用 MINISQL_TEMP_DIR 指定，默认放在数据库文件旁边的隐藏目录。
    if (const char* configured = std::getenv("MINISQL_SESSION_ID"); configured && *configured) sessionId_ = configured;
    // 会话编号（由宿主进程通过环境变量传入）。
    if (const char* configured = std::getenv("MINISQL_CANCEL_FILE"); configured && *configured) cancelFile_ = configured;
    // 取消令牌文件路径，同样是宿主传入。
    // 页替换日志（指导书"替换日志输出"）：设置后每次淘汰追加一行到该文件。
    // 这是指导书要求的能力：把缓冲池的每次淘汰记下来，便于演示与验收。
    // 页替换日志（指导书"替换日志输出"）：设置后每次淘汰追加一行到该文件。
    if (const char* configured = std::getenv("MINISQL_BUFFER_LOG"); configured && *configured) buffer_.setEvictionLog(configured);
    // 启用淘汰日志。
    if (const char* configured = std::getenv("MINISQL_BACKGROUND_CHECKPOINT_MS")) {
    // 后台检查点的调度间隔。
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 3600000) backgroundCheckpointMs_ = static_cast<std::size_t>(parsed);
        // 上限一小时。
    }
    // 后台间隔处理结束。
    const bool forceIndexRebuild = std::getenv("MINISQL_REBUILD_INDEXES") != nullptr;
    for (const auto& table : catalog_.tables()) initializeIndexes(table.id, forceIndexRebuild, true);
    if (backgroundCheckpointMs_ > 0) {
    // 配了间隔才启动后台线程。
        scheduler_ = std::thread([this] { backgroundSchedulerLoop(); });
        // 起一个专用线程跑检查点调度循环。
    }
    // 后台线程判断结束。
}
// Database 构造函数结束。
Database::~Database() {
    persistWorkload();
    // 析构前把本进程的查询负载落盘，否则本次会话的读负载会随进程消失。
// 析构：如果起过后台调度线程，先把它干净地停掉再释放其它成员。
    if (backgroundCheckpointMs_ > 0) {
    // 只有真的起过线程才需要停。
        {
        // 用一个内层作用域把锁的范围限制住。
            std::lock_guard<std::mutex> lock(schedulerMutex_);
            // 拿调度线程的互斥锁，避免与它的等待逻辑竞争。
            schedulerStop_.store(true);
            // 置停止标志（原子写，线程能立刻看到）。
        }
        // 锁在这里释放，避免持有锁去唤醒线程造成死锁。
        schedulerCv_.notify_all();
        // 唤醒可能在等待的调度线程，让它看到停止标志后退出循环。
        if (scheduler_.joinable()) scheduler_.join();
        // 等线程真正结束——必须先 join 再让其它成员析构，否则线程可能访问已释放的对象。
    }
    // 后台线程处理结束。
}
// Database 析构函数结束。

void Database::synchronizeAccessCatalog(const nlohmann::json& document, std::uint32_t permissionVersion) {
// 把外部（页式文件侧）的权限目录同步进引擎的系统表。
// 这是"bridge 与引擎各有权限副本"这套设计的对齐点。
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局递归锁，与其它公共入口串行。
    requireAvailable();
    // 数据库不可用就直接报错。
    if (transaction_ != TransactionState::Idle)
    // 事务进行中不允许改权限目录。
        throw MiniSqlError(ErrorCode::Transaction, "Cannot synchronize access catalog during an active transaction");
        // 因为权限变更会改系统表页，插进事务中间会破坏事务的原子性。
    const auto payload = document.dump();
    // 序列化要写入的权限目录。
    const auto existing = catalog_.accessCatalogRecord();
    // 取出引擎侧现有的权限记录。
    if (existing && existing->permissionVersion >= permissionVersion) {
    // 引擎侧版本不比要写入的旧，说明无需更新。
        if (existing->permissionVersion == permissionVersion && existing->payload != payload)
        // 版本相同但内容不同。
            throw MiniSqlError(ErrorCode::Catalog, "Access catalog version conflict");
            // 这是无法自动裁决的冲突，必须让人工介入。
        return;
        // 同版本同内容属于重复同步，直接返回。
    }
    // 版本比较结束。
    buffer_.beginWriteBatch();
    // 开一个写批次：这一串改动要么整体生效要么整体回滚。
    try {
    // 写系统表可能失败。
        catalog_.storeAccessCatalog(permissionVersion, payload);
        // 把权限目录以带版本的分片写进系统堆表。
        buffer_.commitWriteBatch();
        // 提交这批改动（内部包含先写日志再落盘的链路）。
        catalog_.reload();
        // 重新加载目录，让内存视图与磁盘一致。
    } catch (...) {
    // 任何失败都要把写批次收干净。
        if (file_->writeBatchActive()) buffer_.rollbackWriteBatch();
        // 批次还开着就回滚，避免后续操作被卡在"有未完成批次"的状态里。
        throw;
        // 把原始异常继续抛出。
    }
    ++catalogVersion_;
    // 目录版本加一，用于让上层（如 planner 缓存）知道目录变了。
}

void Database::setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile) {
// 设置会话上下文：会话编号与取消令牌文件。
    sessionId_ = sessionId;
    // 记下会话编号（会出现在日志与审计里）。
    cancelFile_ = cancelFile;
    // 记下取消令牌文件路径；空路径表示这个会话不支持取消。
}
void Database::requireAvailable() const {
// 公共入口的前置检查：数据库必须处于可用状态。
    if (unavailable_) throw MiniSqlError(ErrorCode::Storage, "Database unavailable; reopen for recovery");
    // 内部标记为不可用时明确报错，并提示需要重新打开以走恢复。
    file_->requireHealthy();
    // 再问一次页文件：底层可能因为磁盘或日志损坏而自认为不健康。
}
void Database::checkCancelled() const {
// 检查是否收到了取消请求。
    if (cancelFile_.empty()) return;
    // 没有配置取消文件说明这个会话不支持取消，直接返回。
    std::error_code error;
    // 用 error_code 版本的文件查询，避免文件不存在时抛异常。
    const auto cancelled = std::filesystem::exists(cancelFile_, error);
    // 查文件是否出现（取消是靠"创建标记文件"传递的）。
    if (error) throw MiniSqlError(ErrorCode::Storage, "Cannot inspect cancellation token");
    // 查询本身出错说明环境有问题，按存储错误上报而不是当成"没取消"。
    if (cancelled) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
    // 文件存在即视为收到取消：抛专门的 Cancelled 错误码，
    // 让上层能把它与真正的失败区分开。
}
optimizer::Options Database::optimizerOptions() {
// 构造优化器选项：把改写预算与表行数交给优化器。
    optimizer::Options options;
    options.memoryBudgetBytes = queryMemoryBytes_;
    // 内存预算直接沿用查询级配额。
    for (const auto& table : catalog_.tables()) {
    // 逐张表填行数估算。
        options.tableRows[key(table.definition.table)] = cachedTableRows(table.id, table.definition);
        // 关键改动：改走行数缓存。
        // 原实现每次都用 heap_.scan 把整张表读一遍只为数行数，
        // 而这个函数在每条 SELECT 编译时都会被调用，
        // 等于把每条查询都拖成“全库全表扫描”。
        // 现在只有缓存未命中时才扫一次，写语句会使其失效。
    }
    for (const auto& definition : catalog_.tables()) {
    // 逐张表收集索引列信息，供优化器判断能否用索引顺序替代排序。
        const auto* table = catalog_.view().find(definition.definition.table);
        // 从目录视图取表定义（含索引列表）。
        if (!table || table->indexes.empty()) continue;
        // 没有索引就不必记录。
        std::vector<std::pair<std::string, std::vector<std::string>>> entries;
        // 该表的索引列表：索引名 + 列名。
        for (const auto& index : table->indexes) entries.emplace_back(index.name, index.columns);
        // 逐条索引收进列表。
        options.tableIndexColumns[key(definition.definition.table)] = std::move(entries);
        // 以表名（大小写归一）为键存进选项。
    }
    // 索引元数据收集结束。
    if (const char* configured = std::getenv("MINISQL_DISABLE_RULES")) {
    // 演示用开关：按名字关闭若干优化规则，便于对比优化前后的效果。
    // 多个规则名用逗号分隔；名字与 ruleDescriptors() 里的 ruleId 一致。
        std::string text(configured);
        // 复制一份环境变量内容，便于切分。
        std::size_t begin = 0;
        // 当前片段起点。
        while (begin <= text.size()) {
        // 逐个逗号切分。
            const auto end = text.find(',', begin);
            // 找下一个分隔符。
            const auto piece = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            // 取出这一段规则名。
            if (!piece.empty()) options.disabledRules.push_back(piece);
            // 非空就收进禁用名单。
            if (end == std::string::npos) break;
            // 没有更多分隔符就结束。
            begin = end + 1;
            // 继续下一段。
        }
        // 切分结束。
    }
    // 规则禁用解析结束。
    return options;
}
std::uint64_t Database::cachedTableRows(std::uint64_t tableId, const sql::Statement& definition) {
// 取表行数：先查缓存，命中直接返回；未命中才扫一次并记入。
    const auto found = rowCountCache_.find(tableId);
    // 先在缓存里查这张表。
    if (found != rowCountCache_.end()) { ++rowCountCacheHits_; return found->second; }
    // 命中：命中计数加一，直接返回缓存值，不再触磁盘。
    std::uint64_t rows = 0;
    // 未命中：从 0 开始数。
    heap_.scan(tableId, rowSchema(definition), [&](storage::RowRef, const storage::Row&) { ++rows; });
    // 扫一遍堆表把行数数出来。
    ++rowCountCacheMisses_;
    // 未命中计数加一。
    rowCountCache_[tableId] = rows;
    // 结果写进缓存，下次直接用。
    return rows;
    // 返回行数。
}
void Database::invalidateRowCountCache() {
// 失效行数缓存：任何成功写入都会改变行数。
    rowCountCache_.clear();
    liveStatsCache_ = nullptr;
    // 统计缓存一并失效：写入会改变行数、最值、直方图等全部统计量。
    // 直接清空：写语句数量远少于读，清空比逐项维护更简单也更不容易出错。
    queryResultCache_.clear();
    persistWorkload();
    // 顺便把本次会话累积的负载落盘：写语句是天然的落盘时机，
    // 不用额外开迟线程，也不会在每条查询后都写磁盘。
    // 结果缓存：数据变了，缓存的结果立即过期，必须全部扔掉。
}

nlohmann::json Database::cachedLiveTableStatistics() {
// 取实时表统计：命中直接返回缓存，未命中才做一次全表扫描。
    if (!liveStatsCache_.is_null()) { ++liveStatsCacheHits_; return liveStatsCache_; }
    // 命中：直接返回上次算好的结果，不再触磁盘。
    ++liveStatsCacheMisses_;
    // 未命中计数加一。
    liveStatsCache_ = liveTableStatistics();
    // 真正执行一次全表统计（这是贵的那一步）。
    return liveStatsCache_;
    // 返回结果，同时已经写进缓存。
}
nlohmann::json Database::compile(const std::string& source) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局锁。
    requireAvailable();
    // 可用性检查。
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
    // 事务已中止时不允许再编译新语句——必须先回滚，否则语义上说不清这条语句归属哪个状态。
    const auto tokens = sql::tokenize(source);
    // 词法分析。
    const auto ast = sql::parse(tokens);
    // 语法分析。
    const auto plans = sql::compilePlans(ast, catalog_.view());
    // 语义校验 + 生成逻辑计划（用当前目录做列绑定）。
    const auto optimized = optimizer::optimize(plans, optimizerOptions());
    // plan[*].checkDefinitions 必须与 ast 里的 checks 逐节点相等
    // （tests/check-process.mjs 的 checkDefinitions == ast.checks 断言依赖这一点）。
    // ast 侧的节点由 serializeAst 内的 annotateAst 分配 nodeId/sourceSpan，
    // 而 checkDefinitions 由 serializePlans 单独序列化、拿不到同一套编号。
    // 两者本是同一批表达式树的副本，这里用已标注的 ast 侧覆盖计划侧，保证两份产物一致。
    auto astStatements = sql::serializeAst(ast);
    // serializeAst 对单条语句返回裸对象、多条语句才返回数组，这里统一成数组。
    if (!astStatements.is_array()) astStatements = nlohmann::json::array({astStatements});
    auto planRows = sql::serializePlans(plans);
    auto optimizedRows = sql::serializePlans(optimized.plans);
    for (std::size_t i = 0; i < astStatements.size(); ++i) {
        if (!astStatements[i].is_object() || !astStatements[i].contains("checks")) continue;
        if (i < planRows.size() && planRows[i].is_object()) planRows[i]["checkDefinitions"] = astStatements[i]["checks"];
        if (i < optimizedRows.size() && optimizedRows[i].is_object()) optimizedRows[i]["checkDefinitions"] = astStatements[i]["checks"];
    }
    return {{"success", true}, {"plan", planRows}, {"optimizedPlan", optimizedRows},
             // 返回优化前与优化后的两套计划，便于对照。
            {"optimizationRules", optimized.changes}, {"statements", ast.size()},
             // 命中的优化规则与语句条数。
            {"optimizer", {{"iterations", optimized.iterations}, {"converged", optimized.converged},
                           // 优化器状态：迭代轮数、是否收敛，
                           {"diagnostics", optimized.diagnostics}, {"rules", optimizer::ruleDescriptors()}}},
                           // 诊断信息以及全部可用规则的描述。
            {"tokens", sql::serializeTokens(tokens)}, {"ast", sql::serializeAst(ast)},
             // token 流与语法树原文（前端可以用来展示每一阶段的产物）。
            {"schemaVersion", 1}, {"planKind", "logical"},
             // 协议版本与"这是逻辑计划"的标记。
            {"stages", {{"lexer", "passed"}, {"parser", "passed"}, {"semantic", "passed"},
                         // 各阶段状态：词法、语法、语义都已通过，
                        {"planner", "passed"}, {"optimizer", "passed"}, {"executor", "notRun"}}}};
                         // 计划与优化也通过；执行阶段标记为 notRun（因为这是只编译）。
}

security::AccessRequest Database::bindAccess(const std::string& source) const {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局锁。
    requireAvailable();
    // 可用性检查。
    // 资源上限先于授权：超限的批量输入应报资源超限，而不是权限失败。
    enforceBatchStatementBudget(source);
    // 名称解析只发生在绑定器里。别名、派生表别名、CTE 名都是作用域名，
    // 不会产生受权对象；任何无法闭合的引用都让 bound 保持 false。
    return sql::bindSource(source, catalog_.view()).accessRequest();
}

std::vector<std::string> Database::resolveAccessObjects(const std::string& source) const {
    const auto request = bindAccess(source);
    std::vector<std::string> names;
    if (!request.bound) return names;
    for (const auto& object : request.objects)
        if (std::find(names.begin(), names.end(), object.object) == names.end()) names.push_back(object.object);
    return names;
}
nlohmann::json Database::catalog() {
// 导出目录：把内存目录与实时的行数/页数信息合成一份可读的元数据。
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局锁。
    requireAvailable();
    // 可用性检查。
    json tables = json::array();
    // 结果里的表列表。
    for (const auto& table : catalog_.tables()) {
    // 逐张表处理。
        std::uint64_t rowCount = 0;
        // 行数计数器。
        heap_.scan(table.id, rowSchema(table.definition), [&](storage::RowRef, const storage::Row&) { ++rowCount; });
        // 扫描一遍堆表数行数——目录接口要给出真实数据量而不是估计值。
        json columns = json::array();
        // 列描述列表。
        for (const auto& column : table.definition.columns)
        // 逐列输出。
            columns.push_back({{"name", column.name}, {"type", key(column.type)}, {"nullable", column.nullable}, {"primaryKey", column.primaryKey}, {"unique", column.unique},
                // 列名、类型、可空性、主键与唯一标记，
                {"defaultValue", column.defaultValue ? json(*column.defaultValue) : json(nullptr)}, {"references", sql::serializeReference(column.references)}});
                // 默认值（没有就 null）与列级外键。
        json indexes = json::array();
        // 索引描述列表。
        if (const auto* definition = catalog_.view().find(table.definition.table))
        // 在当前目录视图里找这张表（拿它的索引定义）。
            for (const auto& index : definition->indexes) {
            // 逐个索引输出。
                nlohmann::json entry{{"name", index.name}, {"columns", index.columns}, {"unique", index.unique},
                    // 索引名、索引列、是否唯一，
                    {"storage", "page-file"}, {"pageCount", 0}, {"height", 1}};
                    // 以及存储形态占位（页级）、页数与树高（下面从运行期索引补真实值）。
                for (const auto& runtime : indexes_)
                // 在已打开的运行期索引里找它。
                    if (key(runtime->table) == key(table.definition.table) && key(runtime->name) == key(index.name)) {
                    // 表名与索引名都匹配。
                        entry["pageCount"] = file_->pagesFor(indexOwnerId(runtime->table, runtime->name)).size();
                        // 补上真实占用的页数（按索引的所有者编号查）。
                        entry["height"] = runtime->height();
                        // 补上真实树高。
                    }
                    // 匹配判断结束。
                indexes.push_back(std::move(entry));
                // 收进索引列表。
            }
            // 索引遍历结束。
        tables.push_back({{"name", table.definition.table}, {"tableId", table.id}, {"columns", columns}, {"indexes", indexes}, {"keys", sql::serializeKeys(table.definition.keys)},
            // 表名、内部编号、列、索引与键约束，
            {"foreignKeys", sql::serializeForeignKeys(sql::allForeignKeys(table.definition))},
            // 外键（把列级与表级合并后输出，这样用户在一个地方就能看全）。
            {"checks", sql::serializeChecks(table.definition.checks)},
            // CHECK 约束表达式。
            {"constraintNames", sql::serializeConstraintNames(table.definition.constraintNames)},
            // 约束命名绑定。
            {"rowCount", rowCount}, {"allocatedPages", file_->pagesFor(table.id).size()}});
            // 真实行数与分配给这张表的页数。
    }
    // 表遍历结束。
    return {{"tables", tables}, {"buffer", bufferStatus()}, {"schemaVersion", catalog_.catalogSchemaVersion()}};
    // 返回表列表、缓冲池状态与目录结构版本。
}
nlohmann::json Database::checkpoint() {
// 手动触发一次检查点：把内存里的改动全部落到主文件，并截断日志。
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局锁。
    requireAvailable();
    // 可用性检查。
    if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "CHECKPOINT requires an idle transaction");
    // 有活动事务时不允许做检查点：事务中间落盘会让"未提交"和"已落盘"混在一起。
    // 模糊检查点（MINISQL_FUZZY_CHECKPOINT=1）不强制刷出缓存，只记录检查点边界并保留日志。
    const bool fuzzy = std::getenv("MINISQL_FUZZY_CHECKPOINT") != nullptr;
    const bool archive = std::getenv("MINISQL_ARCHIVE_WAL") != nullptr;
    if (!fuzzy) buffer_.flushAll();
    storage::CheckpointOptions options;
    options.catalogVersion = catalogVersion_;
    options.indexVersion = indexVersion_;
    options.fuzzy = fuzzy;
    options.archive = archive;
        persistWorkload();
        // 检查点顺带把查询负载固化：检查点的语义就是"把当前状态落盘"，
        // 读查询没有写语句那样的落盘时机，靠这里保证建议不会随进程退出而丢。
    file_->checkpoint(options);
    pendingAutoCheckpointWrites_ = 0;
    // 重置"待检查点"的写语句计数——刚做过检查点，这些积累都清了。
    pendingAutoCheckpointWalBytes_ = 0;
    // 同样清掉 WAL 字节积累。
    lastCheckpointAt_ = std::chrono::steady_clock::now();
    // 更新上次检查点时间（单调时钟，用于算间隔）。
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 以及墙上时钟时间戳，
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 用于对外展示。
    return {{"success", true}, {"kind", "Checkpoint"}, {"wal", fuzzy ? "retained" : "truncated"},
            {"fuzzy", fuzzy}, {"archived", archive}};
}

class ScanRowStream : public RowStream {
// 表扫描行流：按需一行行地从堆表里取，不必先把整表读进内存。
public:
// 公开接口：提供构造、取下一行、取消和关闭能力。
    ScanRowStream(storage::HeapStore& heap, std::uint64_t tableId, storage::RowSchema schema)
    // 构造：绑定堆存储、表编号与行结构。
        : heap_(heap), tableId_(tableId), schema_(std::move(schema)) {
        // 前两项存引用/值，行结构用 move 接管。
        refs_ = heap_.refsFor(tableId_);
        reader_ = std::make_unique<storage::HeapStore::RowCursor>(heap_, tableId_, schema_, refs_);
        // 用同一份行引用构造游标：后续读行由游标负责按页取页。
        // 一次性取出这张表所有行的引用（只是引用，不是数据本身）。
        // 这样做的好处是扫描过程中即使有插入/删除也不会让迭代器失效。
    }
    ScanRowStream(storage::HeapStore& heap, std::uint64_t tableId, storage::RowSchema schema,
                  std::vector<storage::RowRef> refs)
        : heap_(heap), tableId_(tableId), schema_(std::move(schema)), refs_(std::move(refs)) {
            reader_ = std::make_unique<storage::HeapStore::RowCursor>(heap_, tableId_, schema_, refs_);
            // 用传入的行引用构造游标。
        }
    bool next(nlohmann::json& row) override {
    // 取下一行。
        if (cancelled_) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        // 被取消就抛专门的取消错误，让上层能区分于普通失败。
        if (cursor_ >= refs_.size()) return false;
        // 已经取完，返回 false。
        storage::Row pending;
        // 游标读出的原始行（强类型），下面再转成 JSON。
        if (!reader_->next(pending)) return false;
        // 从游标取下一行：同一页的连续行不会重复取页。
        row = rowJson(pending);
        // 转成 JSON 返回。
        // 读出这一行并转成 JSON。
        ++cursor_;
        // 游标前移。
        ++rows_;
        // 计数加一（资源用量统计用）。
        return true;
        // 成功取到一行。
    }
    void cancel() override { cancelled_ = true; }
    // 请求取消：只置标记，下一次 next 时抛错。
    void close() override { closed_ = true; }
    // 关闭：本流没有额外资源要释放，只记标记。
    nlohmann::json resourceUsage() const override {
    // 汇报资源用量。
        return {{"kind", "ScanRowStream"}, {"rows", rows_}, {"pending", refs_.size() > cursor_ ? refs_.size() - cursor_ : 0}, {"closed", closed_}};
        // 类型名、已读行数、还剩多少行没读、是否已关闭。
    }
private:
    // 以下为内部状态。
    storage::HeapStore& heap_;
    // 堆存储引用。
    std::uint64_t tableId_;
    // 表编号。
    storage::RowSchema schema_;
    // 行结构（读行时要用它解码）。
    std::vector<storage::RowRef> refs_;
    // 全部行的引用快照（保留用于统计剩余行数与构造游标）。
    std::unique_ptr<storage::HeapStore::RowCursor> reader_;
    // 顺序读游标：同一页的连续行共用一次取页，避免每行都走一次缓冲池查找。
    // 全部行的引用快照。
    std::size_t cursor_ = 0;
    // 当前读到第几条。
    std::size_t rows_ = 0;
    // 已读行数。
    bool cancelled_ = false;
    // 是否被取消。
    bool closed_ = false;
    // 是否已关闭。
};

class FilterRowStream : public RowStream {
// 过滤行流：包住一个子流，只把满足谓词的行放过去。
public:
// 公开接口：子流过滤器的构造与 RowStream 虚函数实现。
    FilterRowStream(std::unique_ptr<RowStream> child, std::function<bool(const nlohmann::json&)> predicate)
    // 构造：接管子流与谓词。
        : child_(std::move(child)), predicate_(std::move(predicate)) {}
        // 两者都用 move 接管。
    bool next(nlohmann::json& row) override {
    // 取下一行：循环向子流要，直到有一行通过谓词。
        while (child_->next(row)) if (predicate_(row)) { ++rows_; return true; }
        return false;
        // 子流取完。
    }
    void cancel() override { cancelled_ = true; child_->cancel(); }
    // 取消要往下传，否则子流（可能是排序、连接）不会停。
    void close() override { child_->close(); }
    // 关闭同样往下传。
    nlohmann::json resourceUsage() const override {
// 返回过滤行流的资源使用信息。
        return {{"kind", "FilterRowStream"}, {"rows", rows_}, {"pending", pending_}, {"cancelled", cancelled_},
            {"child", child_->resourceUsage()}};
    }
private:
// 私有状态：子流、谓词和计数。
    std::unique_ptr<RowStream> child_;
// 上游行流。
    std::function<bool(const nlohmann::json&)> predicate_;
// 过滤谓词。
    std::size_t rows_ = 0;
// 已成功输出的行数。
    bool pending_ = false;
// 是否有一行已从子流取出但尚未输出。
    bool cancelled_ = false;
// 是否已被取消。
};

class ProjectRowStream : public RowStream {
// 投影行流：把每一行按投影表达式改写成新的行。
public:
// 公开接口：投影行流的构造与 RowStream 虚函数实现。
    ProjectRowStream(std::unique_ptr<RowStream> child, std::function<nlohmann::json(const nlohmann::json&)> project)
    // 构造：接管子流与投影函数。
        : child_(std::move(child)), project_(std::move(project)) {}
        // 两者都用 move 接管。
    bool next(nlohmann::json& row) override {
    // 取下一行。
        nlohmann::json input;
        // 先接住子流给的原始行。
        if (!child_->next(input)) return false;
        // 子流取完就结束。
        row = project_(input);
        // 用投影函数算出输出行（可能改变列数、列序，也可能对值做计算）。
        ++rows_;
        // 计数。
        return true;
        // 成功。
    }
    void cancel() override { child_->cancel(); }
    // 取消往下传。
    void close() override { child_->close(); }
    // 关闭往下传。
    nlohmann::json resourceUsage() const override {
    // 资源用量（投影本身不占额外内存，只报行数）。
        return {{"kind", "ProjectRowStream"}, {"rows", rows_}, {"child", child_->resourceUsage()}};
    }
private:
    // 内部状态。
    std::unique_ptr<RowStream> child_;
    // 子流。
    std::function<nlohmann::json(const nlohmann::json&)> project_;
    // 投影函数。
    std::size_t rows_ = 0;
    // 已输出行数。
};

class LimitRowStream : public RowStream {
// 分页行流：先丢弃 offset 行，再最多放行 limit 行。
public:
// 公开接口：分页行流的构造与 RowStream 虚函数实现。
    LimitRowStream(std::unique_ptr<RowStream> child, std::uint64_t offset, std::optional<std::uint64_t> limit)
    // 构造：子流、偏移量、可选的条数上限。
        : child_(std::move(child)), offset_(offset), limit_(limit) {}
        // 参数落位。
    bool next(nlohmann::json& row) override {
    // 取下一行。
        if (limit_ && emitted_ >= *limit_) return false;
        // 已经放满 limit 行就结束（注意 limit 是 optional，没设表示不限制条数）。
        while (skipped_ < offset_) {
        // 还没跳够 offset 行。
            nlohmann::json discarded;
            // 丢弃行需要一个落地变量。
            if (!child_->next(discarded)) return false;
            // 子流提前取完，说明连 offset 都不到，直接结束。
            ++skipped_;
            // 跳过计数加一。
        }
        // 跳过结束。
        if (!child_->next(row)) return false;
        // 取真正的输出行。
        ++emitted_;
        // 输出计数加一。
        return true;
        // 成功。
    }
    void cancel() override { child_->cancel(); }
    // 取消往下传。
    void close() override { child_->close(); }
    // 关闭往下传。
    nlohmann::json resourceUsage() const override {
    // 资源用量。
        return {{"kind", "LimitRowStream"}, {"rows", emitted_}, {"skipped", skipped_},
            {"child", child_->resourceUsage()}};
    }
private:
    // 内部状态。
    std::unique_ptr<RowStream> child_;
    // 子流。
    std::uint64_t offset_ = 0;
    // 要跳过的行数。
    std::optional<std::uint64_t> limit_;
    // 条数上限（空表示不限制）。
    std::uint64_t skipped_ = 0;
    // 已跳过行数。
    std::uint64_t emitted_ = 0;
    // 已输出行数。
};

class MaterializedRowStream : public RowStream {
// 物化行流：数据已经全部在内存里（例如子查询结果），这里只是按游标逐行吐出。
public:
// 公开接口：物化行流的构造与 RowStream 虚函数实现。
    explicit MaterializedRowStream(nlohmann::json rows) : rows_(std::move(rows)) {}
    // 构造：接管现成的行数组。
    bool next(nlohmann::json& row) override {
    // 取下一行。
        if (cursor_ >= rows_.size()) return false;
        // 取完就结束。
        row = rows_.at(cursor_++);
        // 取出当前行并把游标前移。
        return true;
        // 成功。
    }
    void cancel() override { cancelled_ = true; }
    // 取消只置标记（数据已在内存，没有上游需要通知）。
    void close() override { closed_ = true; }
    // 关闭同样只置标记。
    nlohmann::json resourceUsage() const override {
// 返回物化行流的资源使用信息。
        return {{"kind", "MaterializedRowStream"}, {"rows", rows_.size()}, {"emitted", cursor_}, {"closed", closed_}, {"cancelled", cancelled_}};
// 包含总行数、已输出游标、关闭和取消标志。
    }
private:
// 私有状态：行数组与游标。
    nlohmann::json rows_ = nlohmann::json::array();
// 已物化的行数组。
    std::size_t cursor_ = 0;
// 当前输出游标。
    bool cancelled_ = false;
// 是否已被取消。
    bool closed_ = false;
// 是否已关闭。
};

nlohmann::json Database::createSnapshot(const std::filesystem::path& target) {
// 创建数据库快照：把当前页文件复制到目标路径，并返回版本信息。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁，保证复制期间状态稳定。
    requireAvailable();
// 检查实例可用。
    file_->copyTo(target);
// 把页文件内容和检查点信息复制到目标文件。
    const auto& record = file_->checkpointRecord();
// 读取当前检查点记录。
    return {{"success", true}, {"kind", "Snapshot"}, {"target", pathToUtf8(target)},
        {"walBytes", file_->walBytes()}, {"walCutoffBytes", record.walCutoffBytes},
// WAL 字节数与截止位置。
        {"committedSequence", record.committedSequence}, {"catalogVersion", record.catalogVersion},
// 提交序号与目录版本。
        {"indexVersion", record.indexVersion}};
// 索引版本。
}
nlohmann::json Database::indexInspect(const std::string& table, const std::string& index) {
// 查看某个索引的结构与统计，供诊断命令使用。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁。
    requireAvailable();
// 检查实例可用。
    const RuntimeIndex* found = nullptr;
    // 在已打开的运行期索引里找目标索引。
    for (const auto& candidate : indexes_)
    // 逐个候选。
        if (key(candidate->table) == key(table) && key(candidate->name) == key(index)) { found = candidate.get(); break; }
        // 表名与索引名都匹配即命中（大小写不敏感）；用裸指针接住，下面只读不改。
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index not found: " + index);
    // 找不到就按目录错误报出。
    if (!found->pageFile)
    // 用的是内存引擎时，页级结构信息无从谈起。
        return {{"kind", "IndexInspect"}, {"table", found->table}, {"index", found->name}, {"present", false},
                // 明确返回 present=false，
                {"storage", "memory"}, {"message", "page-level structure only available on the page-file engine"}};
                // 并说明"页级结构只在页文件引擎上可用"——这里给结论而不是报错，前端可以直接展示。
    const auto state = found->pageTree->inspect();
    // 页级引擎提供一次结构自检，返回全部页信息与发现的问题。
    std::vector<nlohmann::json> pages;
    // 每页的信息列表。
    pages.reserve(state.pages.size());
    // 预留。
    const auto refJson = [](const storage::PageRef& ref) { return nlohmann::json{{"id", ref.id}, {"generation", ref.generation}}; };
    // 局部工具：把页引用（页号 + 代数）转成 JSON。
    // 代数必须一起输出——只给页号的话，一个被回收又复用的页会看起来像同一个页。
    for (const auto& info : state.pages)
    // 逐页输出。
        pages.push_back({{"page", refJson(info.page)}, {"leaf", info.leaf}, {"height", info.height}, {"keyCount", info.keyCount},
                         // 页引用、是否叶页、树高、键数，
                         {"parent", refJson(info.parent)}, {"left", refJson(info.left)}, {"right", refJson(info.right)}});
                         // 以及父指针与左右兄弟指针。
    nlohmann::json problems = nlohmann::json::array();
    // 检出的问题列表。
    for (const auto& problem : state.problems) problems.push_back(problem);
    // 逐条收进 JSON。
    return {{"kind", "IndexInspect"}, {"table", found->table}, {"index", found->name},
            // 返回类型、表名与索引名，
            {"present", state.present}, {"root", refJson(state.root)}, {"height", state.height},
            // 是否存在、根页引用与树高，
            {"nodeCount", state.nodeCount}, {"leafCount", state.leafCount}, {"rowCount", state.rowCount},
            // 节点数、叶页数与总键数，
            {"leafChainLength", state.leafChainLength}, {"rootReachable", state.rootReachable},
            // 叶链长度与"从根能否走到所有页"，
            {"leafChainLinked", state.leafChainLinked}, {"parentLinksValid", state.parentLinksValid},
            // 叶链是否完整、父指针是否自洽，
            {"storage", "page-file"}, {"problems", std::move(problems)}, {"pages", std::move(pages)}};
            // 存储形态、问题列表与页明细。
}
nlohmann::json Database::indexVerify(const std::string& table, const std::string& index) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    const catalog::StoredTable* stored = nullptr;
    for (const auto& candidate : catalog_.tables())
        if (key(candidate.definition.table) == key(table)) { stored = &candidate; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table not found: " + table);
    RuntimeIndex* found = nullptr;
    for (auto& candidate : indexes_)
        if (key(candidate->table) == key(table) && key(candidate->name) == key(index)) { found = candidate.get(); break; }
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index not found: " + index);
    ++indexVerifications_;
    auto report = verifyIndexConsistency(*found, stored->id, rowSchema(stored->definition));
    report["kind"] = "IndexVerify";
    report["table"] = found->table;
    report["index"] = found->name;
    report["unique"] = found->unique;
    report["storage"] = found->pageFile ? "page-file" : "memory";
    report["height"] = found->height();
    return report;
}
nlohmann::json Database::indexRebuild(const std::string& table, const std::string& index) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    if (transaction_ == TransactionState::Aborted)
        throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
    const catalog::StoredTable* stored = nullptr;
    for (const auto& candidate : catalog_.tables())
        if (key(candidate.definition.table) == key(table)) { stored = &candidate; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table not found: " + table);
    const auto* definition = catalog_.view().find(stored->definition.table);
    if (!definition) throw MiniSqlError(ErrorCode::Catalog, "Index table definition not found");
    const catalog::Index* definitionIndex = nullptr;
    for (const auto& candidate : definition->indexes)
        if (key(candidate.name) == key(index)) { definitionIndex = &candidate; break; }
    if (!definitionIndex) throw MiniSqlError(ErrorCode::Catalog, "Index not found: " + index);
    std::vector<std::size_t> columns;
    for (const auto& name : definitionIndex->columns) columns.push_back(catalog::resolveColumnIndex(*definition, name));
    const auto schema = rowSchema(stored->definition);
    // 在线重建与堆页变更共用写批次：事务内随事务提交/回滚，事务外自成一批。
    const bool ownBatch = transaction_ != TransactionState::Active;
    if (ownBatch) buffer_.beginWriteBatch();
    try {
        // 阶段一 build + 阶段二 validate。
        auto candidate = std::make_unique<RuntimeIndex>(definitionIndex->name, stored->definition.table,
                                                        columns, definitionIndex->unique, pageFileIndexes_);
        std::size_t entries = 0;
        const auto problems = buildIndexEntries(*candidate, stored->id, schema, &entries);
        if (!problems.empty()) {
            const auto detail = "Index rebuild validation failed: " + index + " (" + joinProblems(problems) + ")";
            throw MiniSqlError(ErrorCode::Execution, detail);
        }
        // 阶段三 publish：替换同一索引的运行实例，旧节点页随批次释放。
        indexes_.erase(std::remove_if(indexes_.begin(), indexes_.end(), [&](const auto& runtime) {
            return key(runtime->table) == key(stored->definition.table) && key(runtime->name) == key(index);
        }), indexes_.end());
        const auto height = candidate->height();
        const auto pages = file_->pagesFor(indexOwnerId(stored->definition.table, index)).size();
        indexes_.push_back(std::move(candidate));
        ++indexOnlineRebuilds_;
        ++indexVersion_;
        if (!pageFileIndexes_) persistMemoryIndexes(stored->id);
        if (ownBatch) {
            const auto committedDirtyPages = file_->stagedPageCount();
            buffer_.commitWriteBatch();
            invalidateAnalyzeSnapshot();
            invalidateRowCountCache();
            // 写入已提交：行数可能变了，行数缓存必须一并失效。
            evaluateAutoCheckpoint(1, committedDirtyPages);
        } else {
            ++transactionWriteStatements_;
            invalidateAnalyzeSnapshot();
            invalidateRowCountCache();
            // 写入已提交：行数可能变了，行数缓存必须一并失效。
        }
        return {{"kind", "IndexRebuild"}, {"table", stored->definition.table}, {"index", index},
                {"entries", entries}, {"height", height}, {"pages", pages},
                {"commitState", ownBatch ? "committed" : "pending"}};
    } catch (...) {
        if (ownBatch) rollbackBatch();
        throw;
    }
}
void Database::evaluateAutoCheckpoint(std::size_t committedWriteStatements, std::size_t committedDirtyPages) {
// 在一次成功提交之后评估"是否该做自动检查点"。
    if (committedWriteStatements == 0) return;
    // 没有写语句就没有必要评估（纯读事务不该触发落盘）。
    if (pendingAutoCheckpointWrites_ > std::numeric_limits<std::size_t>::max() - committedWriteStatements)
    // 先判断累加会不会溢出。
        pendingAutoCheckpointWrites_ = std::numeric_limits<std::size_t>::max();
        // 会溢出就直接钉在上限（饱和计数），而不是回绕成一个很小的值。
    else pendingAutoCheckpointWrites_ += committedWriteStatements;
    // 安全时才真的累加。
    const auto committedWalBytes = file_->lastCommitWalBytes();
    // 问页文件：上一次提交写了多少字节日志。
    if (pendingAutoCheckpointWalBytes_ > std::numeric_limits<std::uint64_t>::max() - committedWalBytes)
    // 同样防溢出。
        pendingAutoCheckpointWalBytes_ = std::numeric_limits<std::uint64_t>::max();
        // 饱和处理。
    else pendingAutoCheckpointWalBytes_ += committedWalBytes;
    // 累加。
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : std::min(1.0,
        // 算脏页比例；容量为 0 时按 0 处理，避免除零。
        static_cast<double>(committedDirtyPages) / static_cast<double>(buffer_.capacity()));
        // 顺便夹到 1.0 以内（脏页数理论上不会超过容量，但这样更稳）。
    const auto now = std::chrono::steady_clock::now();
    // 取当前单调时刻。
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpointAt_).count());
    // 算出距上次检查点过了多少毫秒。
    std::vector<std::string> reasons;
    // 触发的理由列表（可能同时满足多条）。
    if (autoCheckpointWrites_ > 0 && pendingAutoCheckpointWrites_ >= autoCheckpointWrites_) reasons.push_back("writes");
    // 写语句数达到阈值。
    if (autoCheckpointWalBytes_ > 0 && pendingAutoCheckpointWalBytes_ >= autoCheckpointWalBytes_) reasons.push_back("wal-bytes");
    // 日志字节数达到阈值。
    if (autoCheckpointDirtyPages_ > 0 && committedDirtyPages >= autoCheckpointDirtyPages_) reasons.push_back("dirty-pages");
    // 脏页数达到阈值。
    if (autoCheckpointDirtyRatio_ > 0.0 && dirtyRatio >= autoCheckpointDirtyRatio_) reasons.push_back("dirty-ratio");
    // 脏页比例达到阈值。
    if (autoCheckpointIntervalMs_ > 0 && elapsedMs >= autoCheckpointIntervalMs_) reasons.push_back("interval");
    // 距上次检查点的时间达到阈值。
    if (reasons.empty()) return;
    // 一条都不满足就什么都不做。
    buffer_.flushAll();
    // 有理由就先把脏页全部刷下去。
    file_->checkpoint({catalogVersion_, indexVersion_});
    // 再做页文件级检查点。
    ++checkpointCount_;
    // 检查点次数加一（对外可观测）。
    pendingAutoCheckpointWrites_ = 0;
    // 清空累计的写语句数。
    pendingAutoCheckpointWalBytes_ = 0;
    // 清空累计的日志字节数。
    lastAutoCheckpointReasons_ = std::move(reasons);
    // 记下这次是哪些理由触发的——诊断接口会展示它，便于解释"为什么突然落盘"。
    lastCheckpointAt_ = now;
    // 更新上次检查点时间。
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 墙上时钟时间戳，
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 用于展示。
    lastAutoCheckpointAtMs_ = lastCheckpointAtMs_;
    // 自动检查点时间与它保持一致。
}
void Database::evaluateBackgroundCheckpoint() {
// 后台线程的评估逻辑：判定条件与提交后那条路径一样，但触发时机不同。
    if (unavailable_) return;
    // 数据库已不可用就不折腾了。
    std::vector<std::string> reasons;
    // 触发理由。
    const auto dirtyPages = buffer_.dirtyPages();
    // 当前脏页数（后台评估看的是"现在"的脏页，而不是本次提交新增的）。
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : std::min(1.0, static_cast<double>(dirtyPages) / static_cast<double>(buffer_.capacity()));
    // 脏页比例，容量为 0 时按 0 处理避免除零。
    const auto now = std::chrono::steady_clock::now();
    // 当前单调时刻。
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpointAt_).count());
    // 距上次检查点过了多久。
    if (autoCheckpointWrites_ > 0 && pendingAutoCheckpointWrites_ >= autoCheckpointWrites_) reasons.push_back("writes");
    // 写语句数达阈值。
    if (autoCheckpointWalBytes_ > 0 && pendingAutoCheckpointWalBytes_ >= autoCheckpointWalBytes_) reasons.push_back("wal-bytes");
    // 日志字节达阈值。
    if (autoCheckpointDirtyPages_ > 0 && dirtyPages >= autoCheckpointDirtyPages_) reasons.push_back("dirty-pages");
    // 脏页数达阈值。
    if (autoCheckpointDirtyRatio_ > 0.0 && dirtyRatio >= autoCheckpointDirtyRatio_) reasons.push_back("dirty-ratio");
    // 脏页比例达阈值。
    if (autoCheckpointIntervalMs_ > 0 && elapsedMs >= autoCheckpointIntervalMs_) reasons.push_back("interval");
    // 时间间隔达阈值。
    schedulerLastEvaluateMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 记下本次评估时间（诊断接口用它证明"调度器还活着"）。
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 墙上时钟时间戳。
    if (reasons.empty()) return;
    // 没有理由就什么都不做。
    // 后台线程仅在空闲且事务空闲时执行检查点，绝不在活动事务提交点之前截断未提交日志。
    // 后台线程仅在空闲且事务空闲时执行检查点，绝不在活动事务提交点之前截断未提交日志。
    if (transaction_ != TransactionState::Idle) { schedulerDeferredReasons_ = std::move(reasons); return; }
    // 事务不空闲就只记下"本可以触发"的理由，等下次再评估。
    buffer_.flushAll();
    // 刷脏页。
    file_->checkpoint({catalogVersion_, indexVersion_});
    // 页文件级检查点。
    ++checkpointCount_;
    // 计数。
    pendingAutoCheckpointWrites_ = 0;
    // 清累计写语句数。
    pendingAutoCheckpointWalBytes_ = 0;
    // 清累计日志字节数。
    lastAutoCheckpointReasons_ = std::move(reasons);
    // 记下触发理由。
    lastCheckpointAt_ = now;
    // 更新上次检查点时间。
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 墙上时钟戳，
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 用于展示。
    lastAutoCheckpointAtMs_ = lastCheckpointAtMs_;
    // 自动检查点时间同步。
    schedulerLastRunMs_ = lastCheckpointAtMs_;
    // 记下调度器"上次真正执行"的时间。
    schedulerDeferredReasons_.clear();
    // 已经执行过了，之前记的"被推迟的理由"就清掉。
}
void Database::backgroundSchedulerLoop() {
// 后台调度线程主循环：定时醒来评估一次，直到收到停止信号。
    std::unique_lock<std::mutex> lock(schedulerMutex_);
    // 用 unique_lock（条件变量等待需要它）。
    while (true) {
    // 一直循环。
        schedulerCv_.wait_for(lock, std::chrono::milliseconds(backgroundCheckpointMs_),
            // 最多睡这么久；
            [&] { return schedulerStop_.load(); });
            // 但一旦停止标志被置位就立刻醒（这就是析构时 notify_all 的意义）。
        if (schedulerStop_.load()) break;
        // 醒来后先看是不是要停，是就跳出循环。
        std::lock_guard<std::recursive_mutex> dbLock(mu_);
        // 拿数据库全局锁——评估与检查点必须与语句执行互斥。
        evaluateBackgroundCheckpoint();
        // 跑一次评估。
    }
    // 循环结束（线程即将退出）。
}
std::string Database::tableFingerprint(std::uint64_t tableId) {
// 算一张表的指纹：把全部行的编码字节与行位置混进一个哈希。
// 用途：判断磁盘上的索引是否还对应当前数据。
    const catalog::StoredTable* stored = nullptr;
    // 找这张表的目录记录。
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    // 按内部编号匹配。
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    // 找不到说明目录与调用方不一致，按目录错误报出。
    std::uint64_t hash = 1469598103934665603ULL;
    // FNV-1a 64 位的标准偏移基数。
    const auto mix = [&](const std::uint8_t* bytes, std::size_t size) {
    // 把一段字节混进哈希。
        for (std::size_t index = 0; index < size; ++index) { hash ^= bytes[index];hash *= 1099511628211ULL; }
        // 逐字节：先异或再乘 FNV 素数。
    };
    // mix 定义结束。
    static constexpr char format[] = "minisql-index-v2";
    mix(reinterpret_cast<const std::uint8_t*>(format), sizeof(format) - 1);
    mix(reinterpret_cast<const std::uint8_t*>(&tableId), sizeof(tableId));
    const auto tableName = key(stored->definition.table);
    mix(reinterpret_cast<const std::uint8_t*>(tableName.data()), tableName.size());
    char buffer[17]{};std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    // 格式化成 16 位定宽十六进制（缓冲区 17 字节刚好容纳结束符）。
    return buffer;
    // 返回指纹文本。
}
std::uint64_t Database::indexOwnerId(const std::string& table, const std::string& index) const {
// 由"表名 + 索引名"算出一个稳定的所有者编号，用于在页文件里区分不同索引的页。
    std::uint64_t hash = 1469598103934665603ULL;
    // FNV 初值。
    const auto mix = [&](const std::uint8_t* bytes, std::size_t size) {
    // 混字节的工具。
        for (std::size_t position = 0; position < size; ++position) { hash ^= bytes[position];hash *= 1099511628211ULL; }
        // 逐字节异或再乘。
    };
    mix(reinterpret_cast<const std::uint8_t*>(table.data()), table.size());
    // 先混表名。
    const std::uint8_t separator = 0;
    // 加一个 0 字节作分隔符。
    mix(&separator, 1);
    // 混进去。
    mix(reinterpret_cast<const std::uint8_t*>(index.data()), index.size());
    // 再混索引名。
    // 分隔符的作用：避免 ("ab","c") 与 ("a","bc") 拼出同一串字节从而撞号。
    return (1ULL << 63) | (hash & ((1ULL << 63) - 1));
    // 强制把最高位置 1：索引页的所有者编号永远落在高半区，
    // 与用户表编号（从 2 开始的小整数）彻底隔开。
}
void Database::clearIndexPages(std::uint64_t owner) {
// 清空某个索引在页文件里占用的所有页。
    const auto pages = file_->pagesFor(owner);
    // 问页文件：这个所有者有哪些页。
    for (const auto& page : pages) buffer_.release(page);
    // 逐页释放——这样页文件可以把它们回收给别人用。
}
void Database::persistIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint) {
// 把内存里的 B+ 树序列化后分片写进页文件（索引的持久化路径）。
    clearIndexPages(owner);
    // 先清掉旧页，避免新旧数据混在一起。
    const auto bytes = tree.dump(fingerprint);
    // 让树自己导出一份带指纹的字节流。
    constexpr std::size_t chunkSize = 3800;
    // 每片 3800 字节——留出页头等开销，保证单片能放进一个 4096 的页。
    const auto total = static_cast<std::uint32_t>((bytes.size() + chunkSize - 1) / chunkSize);
    // 算总片数（向上取整）。
    for (std::uint32_t sequence = 0; sequence < total; ++sequence) {
    // 逐片写入。
        const auto begin = static_cast<std::size_t>(sequence) * chunkSize;
        // 本片起点。
        const auto end = std::min(bytes.size(), begin + chunkSize);
        // 本片终点。
        const auto pageRef = buffer_.allocate(owner);
        // 分配一个新页。
        auto guard = buffer_.get(pageRef);
        // 拿到这个页的写入句柄（guard 析构时会自动把页放回缓冲池）。
        std::vector<std::uint8_t> record;
        // 本页要写的记录。
        record.reserve(32 + end - begin);
        const auto append = [&](std::uint64_t value) {
        // 局部工具：按小端序追加一个 64 位数。
            for (unsigned shift = 0; shift < 64; shift += 8) record.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        };
        record.insert(record.end(), {'I', 'X', 'P', 'A', 'G', 'E', 0, 0});
        // 写 8 字节魔数 "IXPAGE"（后两字节补 0 凑满）。
        append(sequence);append(total);append(static_cast<std::uint64_t>(end - begin));
        // 依次写片序号、总片数、本片长度——读端靠这三个数判断数据是否完整。
        record.insert(record.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        // 追加本片正文。
        guard.insert(record);
        // 把整条记录写进这个页。
    }
    // 分片写入结束。
}
bool Database::loadIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint,
                              std::string* failure) {
    const auto reject = [&](const char* reason) {
        if (failure) *failure = reason;
        return false;
    };
    const auto pages = file_->pagesFor(owner);
    // 取该所有者的页。
    if (pages.empty()) return reject("no snapshot pages");
    std::vector<std::string> chunks;
    // 按片序号收集正文。
    for (const auto& page : pages) {
    // 逐页读取。
        auto guard = buffer_.get(page);
        // 拿页的读句柄。
        const auto slots = guard.page().liveSlots();
        // 取这个页里"活着"的槽位。
        if (slots.size() != 1) return reject("snapshot page slot count");
        const auto record = guard.page().read(slots.front());
        // 读出这条记录。
        if (record.size() < 32 || record[0] != 'I' || record[1] != 'X') return reject("snapshot page header");
        const auto decode = [&](std::size_t offset) {
        // 局部工具：从指定偏移按小端序读一个 64 位数。
            std::uint64_t value = 0;
            // 累加器。
            for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(record[offset + shift / 8]) << shift;
            return value;
            // 返回。
        };
        const auto sequence = decode(8), expectedTotal = decode(16), payloadSize = decode(24);
        // 读片序号、总片数、本片长度。
        if (expectedTotal == 0 || payloadSize > 3800 || sequence >= expectedTotal || record.size() != 32 + payloadSize) {
            if (failure) *failure = "snapshot chunk bounds: sequence=" + std::to_string(sequence) +
                ", total=" + std::to_string(expectedTotal) + ", payload=" + std::to_string(payloadSize) +
                ", record=" + std::to_string(record.size());
            return false;
        }
        if (chunks.size() <= sequence) chunks.resize(static_cast<std::size_t>(sequence) + 1);
        // 按需扩容结果数组。
        if (!chunks[static_cast<std::size_t>(sequence)].empty()) return reject("duplicate snapshot chunk");
        chunks[static_cast<std::size_t>(sequence)].assign(record.begin() + 32, record.end());
        // 取出正文（头是 32 字节）放进对应位置。
    }
    // 逐页读取结束。
    if (chunks.empty() || std::any_of(chunks.begin(), chunks.end(), [](const std::string& chunk) { return chunk.empty(); }))
        return reject("missing snapshot chunk");
    std::string bytes;
    // 拼回来的完整字节流。
    for (auto& chunk : chunks) bytes += chunk;
    // 按序号顺序拼接。
    try { tree.restore(bytes, fingerprint);return true; }
    // 交给树自己反序列化；指纹会一并校验，对不上就抛异常。
    catch (const std::exception& error) {
        if (failure) *failure = error.what();
        return false;
    }
}
void Database::rebuildIndexes(std::uint64_t tableId) {
// 重建某张表的全部索引（打开数据库或数据变动后调用）。
    initializeIndexes(tableId, true, true);
}
void Database::initializeIndexes(std::uint64_t tableId, bool forceRebuild, bool allowRebuild) {
    const catalog::StoredTable* stored = nullptr;
    // 找这张表的目录记录。
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    // 按内部编号匹配。
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    // 找不到就报目录错误。
    const auto* definition = catalog_.view().find(stored->definition.table);
    // 再取一次内存目录里的表定义（索引列表在那里）。
    if (!definition) throw MiniSqlError(ErrorCode::Catalog, "Index table definition not found");
    // 取不到同样报错。
    indexes_.erase(std::remove_if(indexes_.begin(), indexes_.end(), [&](const auto& index) { return key(index->table) == key(stored->definition.table); }), indexes_.end());
    // 先把这张表已有的运行期索引全部摘掉——下面会重新建，避免新旧混在一起。
    // remove_if + erase 是标准的"删除满足条件的元素"写法。
    const auto schema = rowSchema(stored->definition);
    // 取行结构。
    const auto fingerprint = tableFingerprint(tableId);
    // 算一次表指纹（内存引擎用它判断磁盘索引是否还可用）。
    for (const auto& index : definition->indexes) {
    // 逐个索引重建。
        std::vector<std::size_t> columns;
        // 索引列在表内的下标。
        for (const auto& name : index.columns) columns.push_back(catalog::resolveColumnIndex(*definition, name));
        // 把列名解析成下标（顺序即复合索引的顺序）。
        auto runtime = std::make_unique<RuntimeIndex>(index.name, stored->definition.table, columns, index.unique, pageFileIndexes_);
        // 按当前引擎配置构造运行期索引对象。
        const auto owner = indexOwnerId(stored->definition.table, index.name);
        // 算出它在页文件里的所有者编号。
        // 复用路径不重写页：已持久化且结构校验通过（或内存快照可还原）时直接发布。
        bool reusable = false;
        if (pageFileIndexes_) {
        // 页级引擎分支。
            // 页级引擎主路径：每次重建清旧页后从堆全量建树，索引页与堆页同属一个
            // 页级引擎的主路径做法：先清旧页，再从堆表全量建树。
            // PageFile/WAL，随写批次原子落盘/回滚，因而总是与堆一致（无需指纹复用来判断陈旧）。
            // 关键理由：索引页与堆页同属一个 PageFile 与 WAL，
            // 它们随同一个写批次原子落盘或回滚，所以永远与堆数据一致，
            // 也就不需要用指纹去判断索引是否陈旧了。
            runtime->pageTree = std::make_unique<storage::PageBPlusTree>(file_, buffer_, owner, 64, index.unique);
            // 构造页级 B+ 树（64 阶、按索引的唯一性设置）。
            if (!forceRebuild && runtime->pageTree->exists()) {
                try {
                    reusable = runtime->pageTree->validate();
                } catch (const MiniSqlError&) {
                    if (!allowRebuild) throw;
                }
            }
        } else if (!forceRebuild) {
            std::string loadFailure;
            reusable = loadIndexPages(runtime->tree, owner, fingerprint, &loadFailure);
            if (!reusable && !allowRebuild)
                throw MiniSqlError(ErrorCode::Storage, "Index snapshot missing after rollback: " + index.name + " (" + loadFailure + ")");
        }
        // 页级分支结束。
        if (reusable) { indexes_.push_back(std::move(runtime)); continue; }
        if (!allowRebuild) throw MiniSqlError(ErrorCode::Storage, "Index pages missing after rollback: " + index.name);
        // 阶段一 build + 阶段二 validate：候选树在写批次内重建并校验；
        // 失败抛出即随批次回滚，绝不把未通过校验的索引发给优化器。
        std::size_t entries = 0;
        const auto problems = buildIndexEntries(*runtime, tableId, schema, &entries);
        if (!problems.empty())
            throw MiniSqlError(ErrorCode::Execution, "Index build validation failed: " + index.name + " (" + joinProblems(problems) + ")");
        ++indexFullRebuilds_;
        // 阶段三 publish：内存引擎写回快照，页级引擎的节点页已落在 owner 下。
        if (!pageFileIndexes_) persistIndexPages(runtime->tree, owner, fingerprint);
        indexes_.push_back(std::move(runtime));
    }
}
// 三阶段建造的 build + validate：把堆表全量条目写进 index（页级引擎先清空 owner 页），
// 再校验树结构、条目数与唯一性。返回的问题列表为空才算通过；调用方负责 publish。
std::vector<std::string> Database::buildIndexEntries(RuntimeIndex& index, std::uint64_t tableId,
                                                     const storage::RowSchema& schema, std::size_t* entries) {
    std::vector<std::string> problems;
    if (index.pageFile) {
        const auto owner = indexOwnerId(index.table, index.name);
        clearIndexPages(owner);
        index.pageTree = std::make_unique<storage::PageBPlusTree>(file_, buffer_, owner, 64, index.unique);
        if (!index.pageTree->create()) problems.push_back("cannot create index pages");
    } else {
        index.tree.reset();
    }
    std::size_t built = 0, duplicates = 0;
    heap_.scan(tableId, schema, [&](storage::RowRef ref, const storage::Row& row) {
        const auto indexKey = index.keyFor(row);
        if (!RuntimeIndex::indexable(indexKey)) return;
        if (index.insert(indexKey, ref)) ++built;
        else ++duplicates;
    });
    const bool structureValid = index.pageFile ? index.pageTree->validate() : index.tree.validate();
    if (!structureValid) problems.push_back("index structure invalid");
    if (duplicates > 0) problems.push_back("duplicate keys for unique index (" + std::to_string(duplicates) + ")");
    if (index.size() != built) problems.push_back("entry count mismatch (" + std::to_string(index.size()) + " != " + std::to_string(built) + ")");
    if (entries) *entries = built;
    return problems;
}
// 堆表与索引的双向一致性检查：结构、条目数、每条堆行在索引内可达、每个索引条目指向存活且键一致的堆行。
nlohmann::json Database::verifyIndexConsistency(RuntimeIndex& index, std::uint64_t tableId,
                                                const storage::RowSchema& schema) {
    nlohmann::json foundProblems = nlohmann::json::array();
    const auto appendProblem = [&](const std::string& problem) {
        if (foundProblems.size() < 32) foundProblems.push_back(problem);
    };
    const auto sameRow = [](const storage::RowRef& a, const storage::RowRef& b) {
        return a.page.id == b.page.id && a.page.generation == b.page.generation &&
               a.slot.slot == b.slot.slot && a.slot.generation == b.slot.generation;
    };
    bool structureValid = false;
    try {
        structureValid = index.validate();
    } catch (const std::exception&) {
        structureValid = false;
    }
    if (!structureValid) appendProblem("index structure invalid");
    if (index.pageFile) {
        const auto state = index.pageTree->inspect();
        for (const auto& problem : state.problems) appendProblem(problem);
    }
    std::size_t heapRows = 0, indexableRows = 0, missing = 0;
    heap_.scan(tableId, schema, [&](storage::RowRef ref, const storage::Row& row) {
        ++heapRows;
        const auto indexKey = index.keyFor(row);
        if (!RuntimeIndex::indexable(indexKey)) return;
        ++indexableRows;
        const auto matches = index.search(indexKey);
        if (std::none_of(matches.begin(), matches.end(), [&](const storage::RowRef& candidate) { return sameRow(candidate, ref); })) {
            ++missing;
            appendProblem("heap row missing from index");
        }
        // 恢复判断结束。
    });
    const auto allEntries = index.range(std::nullopt, true, std::nullopt, true);
    std::size_t dangling = 0, keyMismatch = 0;
    for (const auto& ref : allEntries) {
        storage::Row row;
        bool alive = true;
        try {
            row = heap_.read(tableId, schema, ref);
        } catch (const std::exception&) {
            alive = false;
        }
        if (!alive) {
            ++dangling;
            appendProblem("index entry points to a missing row");
            continue;
        }
        const auto indexKey = index.keyFor(row);
        if (!RuntimeIndex::indexable(indexKey)) {
            ++dangling;
            appendProblem("index entry points to a non-indexable row");
            continue;
        }
        const auto matches = index.search(indexKey);
        if (std::none_of(matches.begin(), matches.end(), [&](const storage::RowRef& candidate) { return sameRow(candidate, ref); })) {
            ++keyMismatch;
            appendProblem("index entry key mismatch");
        }
    }
    const std::size_t indexRows = allEntries.size();
    return {{"consistent", structureValid && missing == 0 && dangling == 0 && keyMismatch == 0 &&
                            indexRows == indexableRows && index.size() == indexRows},
            {"structureValid", structureValid}, {"heapRows", heapRows}, {"indexableRows", indexableRows},
            {"indexRows", indexRows}, {"runtimeEntries", index.size()}, {"missingEntries", missing},
            {"danglingEntries", dangling}, {"keyMismatches", keyMismatch}, {"problems", std::move(foundProblems)}};
}
void Database::reloadIndexRuntimes() {
    indexes_.clear();
    for (const auto& table : catalog_.tables()) initializeIndexes(table.id, false, false);
    ++indexRuntimeReloads_;
}
void Database::insertIndexEntries(std::uint64_t tableId, const storage::Row& row, storage::RowRef ref) {
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    for (auto& index : indexes_) {
        if (key(index->table) != key(stored->definition.table)) continue;
        const auto indexKey = index->keyFor(row);
        if (!index->insert(indexKey, ref)) throw MiniSqlError(ErrorCode::Execution, "UNIQUE index violation: " + index->name);
        if (RuntimeIndex::indexable(indexKey)) ++indexEntriesInserted_;
    }
}
void Database::eraseIndexEntries(std::uint64_t tableId, const storage::Row& row, storage::RowRef ref) {
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    for (auto& index : indexes_) {
        if (key(index->table) != key(stored->definition.table)) continue;
        const auto indexKey = index->keyFor(row);
        if (!index->erase(indexKey, ref)) throw MiniSqlError(ErrorCode::Storage, "Index entry missing during DML: " + index->name);
        if (RuntimeIndex::indexable(indexKey)) ++indexEntriesErased_;
    }
}
void Database::persistMemoryIndexes(std::uint64_t tableId) {
    if (pageFileIndexes_) return;
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    const auto fingerprint = tableFingerprint(tableId);
    for (auto& index : indexes_) {
        if (key(index->table) == key(stored->definition.table))
            persistIndexPages(index->tree, indexOwnerId(index->table, index->name), fingerprint);
    }
    // 索引遍历结束。
}
void Database::validateUniqueIndexes(std::uint64_t tableId, const storage::Row& row, const std::optional<storage::RowRef>& ignored) {
// 校验一行是否违反唯一索引；ignored 用于 UPDATE 时忽略被改的那一行自身。
    const catalog::StoredTable* stored = nullptr;
    // 找表。
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    // 按编号匹配。
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    // 找不到报错。
    for (const auto& index : indexes_) {
    // 逐个运行期索引检查。
        if (key(index->table) != key(stored->definition.table) || !index->unique) continue;
        // 只关心这张表的唯一索引。
        storage::IndexKey key;
        // 待查的键。
        for (const auto column : index->columns) key.values.push_back(row[column]);
        // 用这一行的值组键。
        if (!RuntimeIndex::indexable(key)) continue;
        const auto matches = index->search(key);
        // 查索引里有没有同键的记录。
        for (const auto& match : matches) {
        // 逐个匹配项。
            if (ignored && match.page.id == ignored->page.id && match.page.generation == ignored->page.generation &&
                // 如果这一项就是被忽略的那一行（四个分量都相同），
                match.slot.slot == ignored->slot.slot && match.slot.generation == ignored->slot.generation) continue;
                // 就跳过——UPDATE 改自己时不该和自己冲突。
            throw MiniSqlError(ErrorCode::Execution, "UNIQUE index violation: " + index->name);
            // 否则就是真正的唯一性冲突。
        }
        // 匹配项遍历结束。
    }
    // 索引遍历结束。
}
// X18: 实时单遍扫描构造各表统计（表/列/索引）。ANALYZE 用它生成快照，
// statistics() 在无快照时也回退到它。
// X18: 实时单遍扫描构造各表统计（表/列/索引）。ANALYZE 用它生成快照，
// statistics() 在无快照时也回退到它。
nlohmann::json Database::liveTableStatistics() {
// 实时统计：一次全表扫描同时算出每列的基数、空值数、最值和分布。
// "单遍"是这里的关键——只扫一次表，而不是为每个统计项各扫一遍。
    json tables = json::array();
    // 结果里的表列表。
    for (const auto& table : catalog_.tables()) {
    // 逐张表统计。
        const auto schema = rowSchema(table.definition);
        // 行结构。
        std::vector<std::set<json>> distinct(schema.size());
        // 每列的不同值集合（算基数用；用 set 天然去重）。
        std::vector<std::uint64_t> nulls(schema.size(), 0);
        // 每列的空值计数，初始为 0。
        std::vector<std::optional<json>> minimum(schema.size());
        // 每列的最小值（用 optional 表示"还没见过任何非空值"）。
        std::vector<std::optional<json>> maximum(schema.size());
        // 每列的最大值。
        std::vector<std::map<std::string, std::pair<json, std::uint64_t>>> frequencies(schema.size());
        // 每列的取值频次：键是值的 JSON 文本，值是（原值，出现次数）。
        // 用 JSON 文本当键是因为 JSON 对象本身不能直接做 map 的键。
        std::uint64_t rowCount = 0;
        // 总行数。
        heap_.scan(table.id, schema, [&](storage::RowRef, const storage::Row& row) {
        // 扫描全表。这一遍就够，所有统计量都在里面更新。
            ++rowCount;
            // 行数加一。
            for (std::size_t index = 0; index < row.size(); ++index) {
            // 逐列处理。
                if (std::holds_alternative<std::monostate>(row[index])) ++nulls[index];
                // 空值只累加空值计数——它不参与基数、最值和频次。
                else {
                // 非空值。
                    auto value = cell(row[index]);
                    // 转成 JSON。
                    distinct[index].insert(value);
                    // 记进该列的不同值集合。
                    if (!minimum[index] || value < *minimum[index]) minimum[index] = value;
                    // 还没记录过，或者新值更小，就更新最小值。
                    if (!maximum[index] || *maximum[index] < value) maximum[index] = value;
                    // 最大值同理。
                    auto& bucket = frequencies[index][value.dump()];
                    // 按值的文本形式找到（或创建）它的计数桶。
                    bucket.first = std::move(value);
                    // 桶里保存一份原值，输出直方图时要用（键只是文本）。
                    ++bucket.second;
                    // 出现次数加一。
                }
                // 非空分支结束。
            }
            // 列遍历结束。
        });
        json columns = json::array();
        // 列统计结果。
        for (std::size_t index = 0; index < table.definition.columns.size(); ++index) {
        // 逐列整理输出。
            std::vector<std::pair<std::string, std::pair<json, std::uint64_t>>> ranked(frequencies[index].begin(), frequencies[index].end());
            // 把频次表拷成可排序的数组。
            std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            // 按出现次数降序排序。
                if (left.second.second != right.second.second) return left.second.second > right.second.second;
                // 次数不同就按次数多的在前。
                return left.first < right.first;
                // 次数相同则按值的文本字典序，保证结果稳定可复现。
            });
            // 排序结束。
            if (ranked.size() > 8) ranked.resize(8);
            // 只保留前 8 个——直方图是给优化器做粗估用的，不需要列出全部取值。
            json histogram = json::array();
            // 高频值直方图。
            json valueHistogram = json::array();
            // 等深分桶直方图。
            for (const auto& [keyValue, entry] : ranked) {
            // 逐个高频值。
                (void)keyValue;
                // 键只是排序用的文本，这里不需要它，显式标记忽略。
                histogram.push_back({{"value", entry.first}, {"count", entry.second}});
                // 输出值与出现次数。
            }
            // 高频值处理结束。
            if (!distinct[index].empty()) {
            // 有非空值才做分桶。
                const std::vector<json> ordered(distinct[index].begin(), distinct[index].end());
                // 把不同值按顺序排好（set 本身有序，转成数组即可）。
                const auto bucketCount = std::min<std::size_t>(8, ordered.size());
                // 最多分 8 桶；不同值不足 8 个时桶数就等于值的个数。
                for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
                // 逐桶统计。
                    const auto begin = bucket * ordered.size() / bucketCount;
                    // 本桶在有序值数组里的起点。
                    const auto end = (bucket + 1) * ordered.size() / bucketCount;
                    // 终点（不含）。
                    if (begin >= end) continue;
                    // 空桶跳过。
                    const auto& lower = ordered[begin];
                    // 桶的下界。
                    const auto& upper = ordered[end - 1];
                    // 桶的上界（含）。
                    std::uint64_t count = 0;
                    // 落在本桶里的行数。
                    for (const auto& [keyValue, entry] : frequencies[index]) {
                    // 遍历该列的所有不同取值。
                        (void)keyValue;
                        // 键不使用。
                        if (!(entry.first < lower) && !(upper < entry.first)) count += entry.second;
                        // 用两次"小于"来判定闭区间 [lower, upper]：
                        // 值不小于下界、且上界不小于值，就说明它落在桶里。
                    }
                    valueHistogram.push_back({{"lower", lower}, {"upper", upper}, {"count", count}});
                    // 记录这一桶的上下界与行数。
                }
                // 分桶结束。
            }
            // 分桶判断结束。
            columns.push_back({{"name", table.definition.columns[index].name},
                // 列名，
                {"columnId", index}, {"type", key(table.definition.columns[index].type)},
                // 列编号与类型，
                {"distinctCount", distinct[index].size()},
                // 基数（不同值个数），优化器估计选择率时最常用的一个数。
                {"nullCount", nulls[index]},
                // 空值数。
                {"nullRatio", rowCount == 0 ? 0.0 : static_cast<double>(nulls[index]) / static_cast<double>(rowCount)},
                // 空值比例；表为空时按 0 处理避免除零。
                {"minValue", minimum[index] ? *minimum[index] : json(nullptr)},
                // 最小值（一列全是空值时给 null）。
                {"maxValue", maximum[index] ? *maximum[index] : json(nullptr)},
                // 最大值。
                {"histogram", std::move(histogram)}, {"valueHistogram", std::move(valueHistogram)}});
                // 两种直方图。
        }
        // 列整理结束。
        json indexes = json::array();
        // 索引统计。
        for (const auto& definition : table.definition.indexes) {
        // 逐个索引输出。
            const RuntimeIndex* runtime = nullptr;
            // 找对应的运行期索引。
            for (const auto& candidate : indexes_) if (key(candidate->table) == key(table.definition.table) && key(candidate->name) == key(definition.name)) { runtime = candidate.get(); break; }
            // 表名与索引名都匹配即命中。
            indexes.push_back({{"name", definition.name}, {"columns", definition.columns}, {"unique", definition.unique},
                // 索引名、索引列与唯一性，
                {"entries", runtime ? runtime->size() : 0}, {"height", runtime ? runtime->height() : 1},
                // 条目数与树高（没打开的运行期索引时给保守值）。
                {"pageCount", runtime && runtime->pageFile ? file_->pagesFor(indexOwnerId(table.definition.table, definition.name)).size() : 0},
                // 页级引擎才统计页数。
                {"statsSource", runtime ? (runtime->pageFile ? "page-bplus-tree" : "memory-bplus-tree") : "missing"}});
                // 明确标出这些数字来自哪种引擎，或者干脆没有（missing）。
        }
        // 索引遍历结束。
        tables.push_back({{"name", table.definition.table}, {"tableId", table.id}, {"rowCount", rowCount},
            // 表名、编号与行数，
            {"allocatedPages", file_->pagesFor(table.id).size()}, {"columns", columns}, {"indexes", std::move(indexes)}});
            // 分配页数、列统计与索引统计。
    }
    // 表遍历结束。
    return tables;
    // 返回统计结果。
}
// ANALYZE 快照的旁路路径：<db>.analyze.json。写语句成功后删除即失效。
// 之所以叫"旁路"：它不占用数据页，而是紧挨着数据库文件放一个同名 .json。
// ANALYZE 快照的旁路路径：<db>.analyze.json。写语句成功后删除即失效。
std::filesystem::path Database::analyzeMetadataPath() const {
// 算出 ANALYZE 快照的文件路径。
    auto path = file_->path();
    // 从页文件拿到数据库文件路径。
    path += ".analyze.json";
    // 直接拼后缀，得到 <db>.analyze.json。
    return path;
    // 返回。
}
std::optional<nlohmann::json> Database::loadAnalyzeMetadata() const {
// 读取 ANALYZE 快照；不存在或内容不合法都返回空。
    std::error_code error;
    // 用 error_code 版本的存在性查询。
    const auto path = analyzeMetadataPath();
    // 路径。
    if (!std::filesystem::exists(path, error) || error) return std::nullopt;
    // 文件不存在（或查询出错）就返回空——快照本来就是可选的。
    try {
    // 读取与解析都可能失败。
        std::ifstream stream(path, std::ios::binary);
        // 二进制方式打开。
        if (!stream) return std::nullopt;
        // 打不开就返回空。
        json document;
        stream >> document;
        // 直接流式解析 JSON。
        if (!document.is_object() || !document.contains("tables") || !document.at("tables").is_array()) return std::nullopt;
        // 结构必须是对象且含 tables 数组，否则当作无效快照。
        return document;
        // 返回快照。
    } catch (const std::exception&) { return std::nullopt; }
    // 任何异常都退回"没有快照"，让调用方走实时统计。
}
void Database::invalidateAnalyzeSnapshot() const {
// 让 ANALYZE 快照失效：直接删文件。
    std::error_code ignored;
    // 删除失败不关心（文件可能本来就不存在）。
    std::filesystem::remove(analyzeMetadataPath(), ignored);
    // 执行删除。
}
nlohmann::json Database::statistics() {
// 对外统计接口：优先用 ANALYZE 快照，没有就现场扫一遍。
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 拿全局锁。
    requireAvailable();
    // 可用性检查。
    // X18: 显式 ANALYZE 的持久快照优先（跨进程有效）；缺失或已被写语句删除时回退实时扫描。
    // 快照是持久的，所以跨进程也有效；一旦有写语句就会把它删掉（见 invalidateAnalyzeSnapshot），
    // 这样统计不会停留在过期数据上。
    // X18: 显式 ANALYZE 的持久快照优先（跨进程有效）；缺失或已被写语句删除时回退实时扫描。
    const auto analyzed = loadAnalyzeMetadata();
    // 尝试读快照。
    const auto tables = analyzed ? analyzed->at("tables") : cachedLiveTableStatistics();
    // 无 ANALYZE 快照时走统计缓存：内容与实时扫描一致，但不必每次重扫。
    // 有快照用快照，否则实时扫描。
    const auto dirtyPages = buffer_.dirtyPages();
    // 脏页数。
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : static_cast<double>(dirtyPages) / static_cast<double>(buffer_.capacity());
    // 脏页比例。
    const auto& record = file_->checkpointRecord();
    // 取页文件里的检查点记录（下面会用到它的字段）。
    const auto wal = file_->walStatistics();
    const auto analyzedAtMs = analyzed ? analyzed->value("analyzedAtMs", std::uint64_t{0}) : std::uint64_t{0};
    // 快照的产生时间；没有快照就是 0。
    const auto refreshedAt = analyzed ? analyzedAtMs : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        // 快照的时间就是它的产生时间；
        std::chrono::system_clock::now().time_since_epoch()).count());
        // 实时扫描的话，"刷新时间"就是现在。
    return {{"success", true}, {"tables", tables}, {"scope", "table-column-index"},
        // 返回结果：表统计、统计范围（表-列-索引三级），
        {"source", analyzed ? "analyze" : "on-demand-scan"},
        // 以及数据来源（显式 ANALYZE 还是临时扫描）——这一项让调用方知道数字的可信度。
        {"statisticsVersion", 1}, {"refreshedAt", refreshedAt}, {"histogramBuckets", 8},
// 统计版本、刷新时间和直方图桶数。
        {"generatedAtMs", refreshedAt}, {"lastAnalyzeAtMs", analyzedAtMs},
// 生成时间与最近一次 ANALYZE 时间。
        {"checkpointCount", checkpointCount_}, {"autoCheckpointWrites", autoCheckpointWrites_},
// 检查点次数与自动检查点触发计数。
        {"autoCheckpointWalBytes", autoCheckpointWalBytes_}, {"autoCheckpointDirtyPages", autoCheckpointDirtyPages_},
// 自动检查点的 WAL 字节数与脏页数阈值。
        {"autoCheckpointDirtyRatio", autoCheckpointDirtyRatio_}, {"autoCheckpointIntervalMs", autoCheckpointIntervalMs_},
// 自动检查点的脏页比例与检查间隔。
        {"pendingAutoCheckpointWrites", pendingAutoCheckpointWrites_}, {"pendingAutoCheckpointWalBytes", pendingAutoCheckpointWalBytes_},
// 当前待触发自动检查点的写语句数与 WAL 字节数。
        {"walBytes", file_->walBytes()}, {"dirtyPages", dirtyPages}, {"dirtyPageRatio", dirtyRatio},
// 当前 WAL 字节数、脏页数与脏页比例。
        {"committedSequence", file_->committedSequence()}, {"dirtyWatermark", file_->dirtyWatermark()},
// 已提交序号与脏页水位。
        {"checkpointRecord", {{"present", record.present}, {"walCutoffBytes", record.walCutoffBytes},
// 检查点记录：是否存在、WAL 截止位置。
            {"dirtyWatermark", record.dirtyWatermark}, {"catalogVersion", record.catalogVersion},
// 检查点记录中的脏页水位与目录版本。
            {"indexVersion", record.indexVersion}, {"committedSequence", record.committedSequence},
// 索引版本与提交序号。
            {"timestampMs", record.timestampMs}, {"walLsn", record.walLsn},
            {"checkpointBeginLsn", record.checkpointBeginLsn}, {"checkpointEndLsn", record.checkpointEndLsn},
            {"archivedBytes", record.archivedBytes}, {"archiveSegments", record.archiveSegments}}},
        // 记录级 WAL 统计：逻辑 LSN 链、事务/记录类型计数与归档状态。
        {"wal", {{"walBytes", wal.walBytes}, {"nextLsn", wal.nextLsn}, {"lastExtentLsn", wal.lastExtentLsn},
            {"lastCheckpointLsn", wal.lastCheckpointLsn}, {"committedSequence", wal.committedSequence},
            {"dirtyWatermark", wal.dirtyWatermark}, {"committedExtents", wal.committedExtents},
            {"abortedExtents", wal.abortedExtents}, {"recordedPages", wal.recordedPages},
            {"trackedPages", wal.trackedPages}, {"archivedBytes", wal.archivedBytes},
            {"archiveSegments", wal.archiveSegments}, {"pendingCommitBytes", wal.pendingCommitBytes},
            {"groupCommit", wal.groupCommit}, {"doubleWrite", wal.doubleWrite},
            {"fuzzyCheckpoint", wal.fuzzyCheckpoint}}},
        {"lastCheckpointAtMs", lastCheckpointAtMs_}, {"lastAutoCheckpointAtMs", lastAutoCheckpointAtMs_},
// 最近一次手动检查点与自动检查点时间。
        // 缓存可观测指标：把行数缓存与统计缓存的命中情况暴露出来。
        // 没有这组指标，使用者只能看到“变快了”却说不清为什么；
        // 有了它就能直接对比：命中率越高，重复扫描越少。
        {"queryCache", queryCacheDocument()},
        // 索引建议也一并返回：负载统计存在进程内存里，
        // 独立进程调 indexAdvisor 模式读不到它，但 statistics 可以同进程取。
        {"indexAdvisor", buildIndexAdvisor()},
        // 缓存可观测指标：行数缓存、统计缓存与页缓存的命中情况。
        // 单独抽成一个函数构造，避免在这个已经很长的初始化列表里
        // 再套一层嵌套字典，那样很容易写错括号层级。
        // 容量与当前驻留页数，看得出缓存是否吃紧。
        {"lastAutoCheckpointReasons", lastAutoCheckpointReasons_},
        // 最近一次自动检查点的触发原因列表。
        {"indexMaintenance", {{"engine", pageFileIndexes_ ? "page-file" : "memory"},
            {"fullRebuilds", indexFullRebuilds_}, {"runtimeReloads", indexRuntimeReloads_},
            {"entriesInserted", indexEntriesInserted_}, {"entriesErased", indexEntriesErased_}}},
        {"backgroundScheduler", {{"enabled", backgroundCheckpointMs_ > 0 && scheduler_.joinable()},
// 后台调度器状态：是否启用、间隔与最近评估情况。
            {"intervalMs", backgroundCheckpointMs_}, {"lastEvaluateMs", schedulerLastEvaluateMs_},
// 评估间隔与最近一次评估时间的时间戳。
            {"lastRunMs", schedulerLastRunMs_}, {"deferredReasons", schedulerDeferredReasons_}}}};
// 最近一次真正执行时间，以及这一轮被推迟的原因。
}
std::string Database::queryResultCacheKey(const std::vector<sql::Token>& statement, bool optimize) const {
// 把一条语句的 token 归一化成缓存键。
// 归一化做两件事：关键字与标识符统一转小写、各 token 用不会出现在 SQL 里的分隔符接起来。
// 这样 "select id from t" 与 "SELECT  ID  FROM  T" 会得到同一个键。
    std::string key;
    // 累加结果。
    key += optimize ? "opt|" : "raw|";
    // 把是否走优化器编进键：同一条 SQL 在两种模式下计划不同。
    for (const auto& token : statement) {
    // 逐个 token 追加。
        if (token.type == "END") continue;
        // 结束标记不参与。
        std::string piece = token.lexeme;
        // 取词素。
        for (char& c : piece) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // 全部转小写：关键字与标识符都不区分大小写。
        key += piece;
        // 追加进键。
        key += static_cast<char>(0x1f);
        // 用一个不会出现在 SQL 里的分隔符，避免 "ab"+"c" 与 "a"+"bc" 撞键。
    }
    return key;
    // 返回缓存键。
}
void Database::recordWorkload(const sql::LogicalPlan& plan) {
// 记录一棵计划里的谓词列，供索引建议器累积查询习惯。
// 只认“过滤直接架在扫描上”这种形态：那正是全表扫描慢在哪里、
// 建索引能直接改善的场景。已经走上索引的查询不再计入。
    std::function<void(const nlohmann::json&, const std::string&, std::size_t)> collect;
    // 先声明再赋值：lambda 在自己的初始化式里引用自己是不合法的，
    // 必须先用 std::function 声明一个变量，再把带递归调用的 lambda 赋给它。
    collect = [&](const nlohmann::json& expression, const std::string& table, std::size_t depth) {
    // 递归扫描一棵谓词表达式树。
        if (depth > 64 || !expression.is_object() || table.empty()) return;
        // 深度与类型保护；没有表名时无法归属。
        const auto kind = expression.value("kind", std::string{});
        // 节点种类。
        if (kind == "Binary") {
        // 二元比较才可能是可用索引的谓词。
            const auto op = expression.value("operator", std::string{});
            // 取运算符。
            const bool equality = op == "=";
            // 等值条件。
            const bool range = op == "<" || op == "<=" || op == ">" || op == ">=";
            // 范围条件。
            if (equality || range) {
            // 只对这两类计数。
                const auto note = [&](const nlohmann::json& side) {
                // 只统计列引用那一侧。
                    if (!side.is_object() || side.value("kind", std::string{}) != "Identifier") return;
                    // 非列引用（比如字面量）不计。
                    const auto name = side.value("name", std::string{});
                    // 列名。
                    if (name.empty()) return;
                    // 名字为空时无法归属。
                    auto& bucket = workload_[key(table)][key(name)];
                    // 取表名、列名对应的累计桶。
                    if (equality) ++bucket.equality;
                    // 等值计数加一。
                    else ++bucket.range;
                    // 范围计数加一。
                };
                // note lambda 结束。
                note(expression.contains("left") ? expression.at("left") : nlohmann::json{});
                // 看左侧。
                note(expression.contains("right") ? expression.at("right") : nlohmann::json{});
                // 看右侧。
            }
            // 可用谓词处理结束。
        }
        // Binary 分支结束。
        if (expression.contains("left")) collect(expression.at("left"), table, depth + 1);
        // 递归左子树（AND / OR 链会走这里）。
        if (expression.contains("right")) collect(expression.at("right"), table, depth + 1);
        // 递归右子树。
    };
    // collect lambda 结束。
    std::function<void(const sql::LogicalPlan&, std::size_t)> walk;
    // 同理：遍历也需要递归，一样用 std::function 声明后再赋值。
    walk = [&](const sql::LogicalPlan& node, std::size_t depth) {
    // 遍历计划树。
        if (depth > 64) return;
        // 计划深度保护。
        if (node.kind == "Filter" && node.children.size() == 1 && node.children.front().kind == "SeqScan") {
        // 关键形态：过滤直接架在全表扫描上，建索引最能受益。
            const auto table = node.children.front().table;
            // 取被扫描的表名。
            const auto& predicate = node.predicate;
            // 取谓词。
            const bool usable = predicate.is_object() && predicate.value("kind", std::string{}) != "Literal";
            // 恒真恒假字面量没有列，不会产生建议。
            if (usable) {
            // 只有真正带列的谓词才记。
                bool hasIndexAny = false;
                // 检查该表是否已有可用索引。
                if (const auto* definition = catalog_.view().find(table))
                // 去目录里找表定义。
                    hasIndexAny = !definition->indexes.empty();
                    // 已有任意索引就不再反复提建议。
                if (!hasIndexAny) collect(predicate, table, 0);
                // 只有表上还没索引时才累积负载。
            }
            // 谓词处理结束。
        }
        // Filter+SeqScan 分支结束。
        for (const auto& child : node.children) walk(child, depth + 1);
        // 递归所有孩子。
    };
    // walk lambda 结束。
    walk(plan, 0);
    // 从根开始遍历。
}
nlohmann::json Database::indexAdvisor() {
// 对外入口：取索引建议。只读，不改数据。
    std::lock_guard<std::recursive_mutex> guard(mu_);
    // 加全局锁，保证读取负载统计时状态稳定。
    requireAvailable();
    // 不可用实例拒绝请求。
    return buildIndexAdvisor();
    // 委托给实现函数。
}
std::filesystem::path Database::workloadPath() const {
// 负载统计的旁路文件：放在数据库文件旁边，名字由数据库路径加后缀得到。
    auto path = file_->path();
    // 取数据库文件路径。
    path += ".workload.json";
    // 拼上后缀。
    return path;
    // 返回路径。
}
nlohmann::json Database::loadWorkload() const {
// 读历史负载；文件不存在或损坏都当作空历史。
    std::error_code error;
    // 用 error_code 版本的存在性查询，避免抛异常。
    const auto path = workloadPath();
    // 取路径。
    if (!std::filesystem::exists(path, error) || error) return nlohmann::json::object();
    // 不存在就返回空对象。
    try {
    // 读取与解析都可能失败。
        std::ifstream stream(path, std::ios::binary);
        // 二进制方式打开。
        if (!stream) return nlohmann::json::object();
        // 打不开就当空。
        nlohmann::json document;
        // 目标对象。
        stream >> document;
        // 流式解析。
        if (!document.is_object()) return nlohmann::json::object();
        // 结构不对就当空。
        return document;
        // 返回读到的文档。
    } catch (const std::exception&) { return nlohmann::json::object(); }
    // 任何异常都退回空历史，不让旁路数据影响主流程。
}
void Database::persistWorkload() {
// 把内存负载并入旁路文件，让后续进程（如 indexAdvisor）能读到。
    if (workload_.empty()) return;
    // 没有新负载就不必写文件。
    auto merged = loadWorkload();
    // 先读历史，在历史基础上累加。
    for (const auto& [table, columns] : workload_) {
    // 逐张表合并。
        auto& bucket = merged[table];
        // 取该表的累计对象（不存在则新建）。
        if (!bucket.is_object()) bucket = nlohmann::json::object();
        // 保证是对象类型。
        for (const auto& [column, counts] : columns) {
        // 逐列合并。
            auto& slot = bucket[column];
            // 取该列的计数。
            if (!slot.is_object()) slot = {{"equality", 0}, {"range", 0}};
            // 新列初始化为 0。
            slot["equality"] = slot.value("equality", std::uint64_t{0}) + counts.equality;
            // 等值次数累加。
            slot["range"] = slot.value("range", std::uint64_t{0}) + counts.range;
            // 范围次数累加。
        }
        // 列合并结束。
    }
    // 表合并结束。
    try {
    // 写文件可能失败（权限、磁盘满）。
        std::ofstream stream(workloadPath(), std::ios::binary | std::ios::trunc);
        // 截断方式打开，整体重写。
        if (!stream) return;
        // 打不开就放弃：旁路统计不值得影响主流程。
        stream << merged.dump();
        workload_.clear();
        // 关键：落盘后必须清空内存计数，否则下一次落盘会把同一批次数再累加一遍，
        // 负载统计会被越放越大，建议随之失真。
        // 写入合并后的 JSON。
    } catch (const std::exception&) { /* 旁路数据写入失败不影响查询本身 */ }
    // 异常吞掉：这只是辅助信息。
}
nlohmann::json Database::buildIndexAdvisor() const {
// 索引建议器：把累积的查询负载整理成可直接采用的建议。
// 数据源是两份合并：历史旁路文件（跨进程）+ 本进程内存里还没落盘的部分。
    nlohmann::json merged = loadWorkload();
    // 先取历史负载快照。
    for (const auto& [table, columns] : workload_) {
    // 把本进程还没落盘的负载并进去。
    // 遍历的是 workload_，修改的是 merged，两者不同对象，
    // 不会出现“边遍历边往同一个 map 插入”的迭代器失效问题。
        auto& bucket = merged[table];
        // 取该表在合并结果里的位置（不存在则新建）。
        if (!bucket.is_object()) bucket = nlohmann::json::object();
        // 防止历史文件损坏导致类型不对。
        for (const auto& [column, counts] : columns) {
        // 逐列累加。
            auto& slot = bucket[column];
            // 取该列的计数对象。
            if (!slot.is_object()) slot = {{"equality", 0}, {"range", 0}};
            // 不存在或类型不对时初始化。
            slot["equality"] = slot.value("equality", std::uint64_t{0}) + counts.equality;
            // 等值次数累加。
            slot["range"] = slot.value("range", std::uint64_t{0}) + counts.range;
            // 范围次数累加。
        }
        // 列累加结束。
    }
    // 本进程负载合并结束。
    nlohmann::json recommendations = nlohmann::json::array();
    // 建议列表。
    for (auto tableEntry = merged.begin(); tableEntry != merged.end(); ++tableEntry) {
    // 显式迭代：MSVC 对 json 的结构化绑定支持不完整，统一改手动取键值。
        const auto& table = tableEntry.key();
        // 表名。
        const auto& columns = tableEntry.value();
        // 已经建有索引的表不再建议：负载是历史累积的，建完索引后旧计数仍在，
        // 不排除的话会一直提示给同一张表重复建索引。
        if (const auto* definition = catalog_.view().find(table))
            if (!definition->indexes.empty()) continue;
        // 该表已有索引，跳过。
        // 该表的列计数对象。
    // 逐张表看。
        if (!columns.is_object()) continue;
        // 结构不对就跳过。
        for (auto entry = columns.begin(); entry != columns.end(); ++entry) {
        // 显式迭代：MSVC 对 json::items() 的结构化绑定支持有问题，改用手动取键值。
            const auto& column = entry.key();
            // 列名。
            const auto& counts = entry.value();
            // 该列的计数对象。
        // 逐个列看。
            if (!counts.is_object()) continue;
            // 列的计数必须是对象。
            const std::uint64_t equality = counts.value("equality", std::uint64_t{0});
            // 等值次数。
            const std::uint64_t range = counts.value("range", std::uint64_t{0});
            // 范围次数。
            const std::uint64_t total = equality + range;
            // 总次数。
            if (total == 0) continue;
            // 没有被访问过就不建议。
            recommendations.push_back({
            // 一条建议。
                {"table", table}, {"column", column},
                // 表名与列名。
                {"equalityScans", equality}, {"rangeScans", range}, {"totalScans", total},
                // 三种计数，让使用者看得出依据。
                {"estimatedSpeedup", total >= 3 ? "high" : total >= 2 ? "medium" : "low"},
                // 粗略分档：访问次数越多，建索引越值得。
                {"suggestedStatement", "CREATE INDEX idx_" + table + "_" + column + " ON " + table + "(" + column + ");"},
                // 直接给可执行的建索引语句，拿来就能用。
            });
            // 收进建议列表。
        }
        // 列遍历结束。
    }
    // 表遍历结束。
    std::sort(recommendations.begin(), recommendations.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    // 按访问次数降序排序，最值得建的排在前面。
        return a.at("totalScans").get<std::uint64_t>() > b.at("totalScans").get<std::uint64_t>();
        // 次数多的在前。
    });
    // 排序结束。
    return {{"success", true}, {"recommendations", recommendations}, {"scope", "filter-over-seqscan-without-index"},
    // 带上 success 字段：与 statistics / catalog 等只读接口保持一致。
    // 返回建议与统计范围，说明这些建议从何而来。
        {"trackedTables", merged.size()}};
        // 跟踪的表数（含历史），为 0 时说明还没有负载。
}
nlohmann::json Database::queryCacheDocument() const {
// 汇总三级缓存的命中情况，供 statistics 对外展示。
// 三级分别是：行数缓存（优化器估算用）、
// 实时统计缓存（EXPLAIN 与 statistics 用）、以及底层页缓存（磁盘 IO 用）。
    const auto& pageStats = buffer_.stats();
    // 取底层页缓存统计。
    const auto requests = pageStats.hits + pageStats.misses;
    // 总请求数，用于算命中率。
    return {
    // 下面逐级组织。
        {"rowCount", {{"hits", rowCountCacheHits_}, {"misses", rowCountCacheMisses_},
        // 行数缓存：命中与未命中次数。
            {"entries", rowCountCache_.size()}, {"scope", "table-row-counts"}}},
        // 当前缓存的表数与缓存范围。
        {"queryResult", {{"enabled", resultCacheEnabled_}, {"hits", queryResultCacheHits_},
        // 结果缓存：命中直接返回已算好的结果，跳过编译与执行。
            {"misses", queryResultCacheMisses_}, {"entries", queryResultCache_.size()},
        // 未命中次数与当前缓存条目数。
            {"maxRows", resultCacheMaxRows_}, {"scope", "single-statement-select-autocommit"}}},
        // 行数上限与缓存范围：只缓存自动提交下的单条 SELECT。
        {"liveStats", {{"hits", liveStatsCacheHits_}, {"misses", liveStatsCacheMisses_},
        // 统计缓存：命中与未命中次数。
            {"cached", !liveStatsCache_.is_null()}, {"scope", "table-column-histograms"}}},
        // cached 直接告诉调用方当前是否持有有效缓存。
        {"bufferPool", {{"hits", pageStats.hits}, {"misses", pageStats.misses},
        // 页缓存的命中与未命中次数。
            {"hitRate", requests == 0 ? 0.0 : static_cast<double>(pageStats.hits) / static_cast<double>(requests)},
        // 命中率：没有请求时按 0 处理，避免除零。
            {"capacity", buffer_.capacity()}, {"residentPages", buffer_.size()}}},
        // 容量与当前驻留页数。
    };
}
nlohmann::json Database::bufferStatus() const {
// 返回当前缓冲池的运行状态快照，供诊断接口和验收脚本读取。
    const auto& stats = buffer_.stats();
// 取出缓冲池累计的命中/未命中与替换计数。
    const auto requests = static_cast<double>(stats.hits) + static_cast<double>(stats.misses);
// 总请求数等于命中数加未命中数，下面用它计算命中率。
    json evictions = json::array();
// 把替换日志整理成 JSON 数组，便于调用方逐条看到页淘汰过程。
    for (const auto& event : buffer_.evictions()) {
// 逐条遍历最近发生的页淘汰事件。
        evictions.push_back({{"sequence", event.sequence},
// 写入淘汰事件序号，作为事件先后顺序的稳定标识。
            {"policy", event.policy == storage::ReplacementPolicy::LRU ? "LRU" : event.policy == storage::ReplacementPolicy::CLOCK ? "CLOCK" : "FIFO"},
// 记录淘汰时采用的替换策略：LRU 或 FIFO。
            {"pageId", event.page.id}, {"generation", event.page.generation},
// 记录被淘汰页的页号和代数，代数用于区分同一页号的旧版本。
            {"dirty", event.dirty}, {"writeBack", event.writeBack}});
// 记录该页淘汰时是否为脏页，以及是否已经写回主文件。
    }
// 单条淘汰事件组装结束，回到循环处理下一条。
    return {{"available", true}, {"scope", "database-instance"},
// 开始组装缓冲池状态返回值：作用域限定在当前数据库实例。
        {"policy", buffer_.policy() == storage::ReplacementPolicy::LRU ? "LRU" : buffer_.policy() == storage::ReplacementPolicy::CLOCK ? "CLOCK" : "FIFO"},
// 当前生效的替换策略。
        {"capacity", buffer_.capacity()}, {"residentPages", buffer_.size()},
// 缓冲池容量与当前常驻页数。
        {"hits", stats.hits}, {"misses", stats.misses},
// 累计命中次数与未命中次数。
        {"hitRate", requests == 0 ? 0 : static_cast<double>(stats.hits) / requests},
// 命中率；没有任何请求时按 0 处理，避免除零。
        {"diskReads", file_->ioStats().reads}, {"diskWrites", file_->ioStats().writes},
// 页文件层累计的磁盘读次数与写次数。
        {"diskScope", "database-file-pages-including-header"},
// 说明磁盘统计包含数据库文件头页，避免口径不一致。
        {"ioErrors", file_->ioStats().errors}, {"stagedPageReads", stats.stagedPageReads},
// 页 IO 错误数与经由暂存区完成的读次数。
        {"stagedPageWrites", stats.stagedPageWrites}, {"evictions", evictions},
        // CLOCK 专用指标：扫描步数与二次机会次数。
        // 有了它才能量化新算法与 LRU 的差异，而不是只停在描述上。
        {"clockSweeps", stats.clockSweeps}, {"clockSecondChances", stats.clockSecondChances}};
// 经由暂存区完成的写次数与前面整理的淘汰日志。
}
// bufferStatus 返回结束。
 nlohmann::json Database::configureBuffer(const std::string& action) {
// 调整缓冲池策略或清零统计，只允许在事务空闲时执行。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加全局递归锁，保证配置修改与并发请求互斥。
    requireAvailable();
// 检查数据库实例仍然可用。
    if (transaction_ != TransactionState::Idle)
// 事务未空闲时不允许切换策略，否则正在执行的事务会突然换替换算法。
        throw MiniSqlError(ErrorCode::Transaction, "Buffer configuration requires an idle transaction");
// 抛出事务状态错误，明确要求先提交或回滚。
    if (action == "LRU") buffer_.setPolicy(storage::ReplacementPolicy::LRU);
// 动作 LRU：切换为最近最少使用替换策略。
    else if (action == "FIFO") buffer_.setPolicy(storage::ReplacementPolicy::FIFO);
// 动作 FIFO：切换为先进入先淘汰策略。
    else if (action == "CLOCK") buffer_.setPolicy(storage::ReplacementPolicy::CLOCK);
    // 动作 CLOCK：切换为二次机会替换策略。
    else if (action == "RESET") buffer_.resetStats();
// 动作 RESET：保留当前策略，只把统计计数清零。
    else throw MiniSqlError(ErrorCode::InvalidArgument, "Expected LRU, FIFO, CLOCK or RESET");
// 其他动作一律视为非法参数，防止静默忽略拼写错误。
    return {{"tables", json::array()}, {"buffer", bufferStatus()}};
// 返回空表列表与更新后的缓冲池状态，方便调用方立即确认效果。
}
// configureBuffer 返回结束。
std::vector<storage::Row> Database::joinRows(const sql::LogicalPlan& plan) {
// 把逻辑计划节点直接执行成行集合，供 JOIN 这类需要物化输入的计划使用。
    checkCancelled();
// 每进入一个节点都先检查取消标志，长查询可以及时停下。
    std::vector<storage::Row> rows;
// 准备承载本节点输出的行集合。
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
// 过滤、半连接、反连接和 Apply 都只有一个输入，先在输入上求值再筛选。
        if (plan.children.size() != 1) fail("Join filter requires one child");
// 这几种计划的语义都要求恰好一个子节点，否则计划树不合法。
        rows = joinRows(plan.children.front());
// 递归把子节点执行成行集合，作为本节点的输入。
        for (auto iterator = rows.begin(); iterator != rows.end();) {
// 用迭代器扫描输入行，允许在循环中安全删除不满足条件的行。
            if (!accepted(evaluate(plan.predicate, *iterator))) iterator = rows.erase(iterator);
// 计算过滤谓词；不接受的行走 erase 并从返回值拿下一个有效迭代器。
            else ++iterator;
// 谓词通过时只推进迭代器，保留这一行。
        }
// 输入行集合筛选结束。
        return rows;
// 返回过滤后的行集合，本分支结束。
    }
    if (plan.kind == "Project") {
// Project 也可以作为 JOIN 的输入出现：FROM (SELECT ...) x JOIN t y 会把派生表
// 规划成 Project(SeqScan)，而这里原本没有 Project 分支，导致 joinRows 走到末尾的
// "Unsupported join input"——派生表 JOIN 因此完全不可用。派生表是带别名的子查询，
// 语义上就是"先算出子查询的行，再按外层列引用取列"，所以这里先递归物化子节点，
// 再对每一行求值投影表达式即可。
        if (plan.children.size() != 1) fail("Join projection requires one child");
// 投影必须恰好有一个输入。
        rows = joinRows(plan.children.front());
// 先物化子输入。
        for (auto& row : rows) {
// 逐行做投影。
            storage::Row projected;
// 投影后的强类型行。
            projected.reserve(plan.projections.empty() ? plan.output.size() : plan.projections.size());
// 按输出列数预留空间。
            if (!plan.projections.empty()) {
// 有显式投影表达式时逐项求值。
                for (const auto& expression : plan.projections) {
// 遍历每个投影表达式。
                    const auto value = evaluate(expression, row);
// 在子输入行上求值。
                    const auto type = expression.value("type", std::string{"int"});
// 取出表达式类型，供下面的转换使用。
                    projected.push_back(indexValue(value, type));
// 转成内部存储值并追加。
                }
// 投影表达式遍历结束。
            } else {
// 没有显式表达式时按输出模式的列号直接取列。
                for (const auto& column : plan.output) {
// 遍历每一输出列。
                    if (column.columnId >= row.size()) fail("Join projection outside row");
// 列号越界说明计划与输入模式不一致。
                    projected.push_back(row.at(column.columnId));
// 复制对应单元格。
                }
// 输出列遍历结束。
            }
// 投影分支结束。
            row = std::move(projected);
// 用投影结果替换原行，保持行数不变。
        }
// 逐行投影结束。
        return rows;
// 返回投影后的行集合。
    }
// 过滤类输入分支结束。
    if (plan.kind == "IndexScan") {
// IndexScan 节点：按索引键或范围取出候选行引用，再回堆表读行。
// IndexScan 不能直接在 joinRows 里访问页结构，转交统一的 runNode 执行。
        const auto result = runNode(plan);
// 调用执行器生成索引扫描结果。
        const auto& indexedRows = result.at("rows");
// 取出结果中的 rows 数组。
        rows.reserve(indexedRows.size());
// 预分配空间，避免逐行 push_back 反复扩容。
        for (const auto& row : indexedRows) {
// 逐行把 JSON 结果转换成内部存储行。
            if (!row.is_array() || row.size() != plan.output.size()) fail("IndexScan join row schema mismatch");
// 检查行是数组且列数与计划输出模式一致。
            storage::Row converted;
// 准备一行强类型结果。
            converted.reserve(row.size());
// 按列数预留空间。
            for (std::size_t index = 0; index < row.size(); ++index) converted.push_back(indexValue(row[index], plan.output[index].type));
// 把第 index 列按计划声明类型转换后放入结果行。
            rows.push_back(std::move(converted));
// 收集这一行。
        }
// 索引结果遍历结束。
        return rows;
// 返回索引扫描产生的行集合。
    }
// IndexScan 分支结束。
    if (plan.kind == "SeqScan") {
// SeqScan 直接走堆表扫描，把每一行读进内存后再参与 JOIN。
        const catalog::StoredTable* table = nullptr;
// 先找到计划里引用的表定义，找不到就无法构造行模式。
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
// 在目录中按表名做大小写不敏感匹配。
        if (!table) fail("Join scan references missing table");
// 计划引用了不存在的表，属于内部计划错误，直接失败。
        heap_.scan(table->id, rowSchema(table->definition), [&](storage::RowRef, const storage::Row& row) {
// 用堆表扫描器逐行读取；第一个参数是行引用，这里不需要所以省略名字。
            checkCancelled();
// 每读一行前检查取消标志，避免大表扫描拖住关闭流程。
            rows.push_back(row);
// 把读到的行追加到结果集合。
        });
// 扫描结束，堆表迭代器析构并释放资源。
        return rows;
// 返回全表扫描得到的行集合。
    }
// SeqScan 分支结束。
    if ((plan.kind != "NestedLoopJoin" && plan.kind != "HashJoin" && plan.kind != "LeftJoin" && plan.kind != "RightJoin" && plan.kind != "FullJoin") || plan.children.size() != 2) fail("Unsupported join input");
// 只接受五种 JOIN 计划且必须恰好两个输入，否则拒绝执行。
    const auto left = joinRows(plan.children[0]);
// 先物化左输入；嵌套循环连接需要反复扫描它。
    if (left.empty() && plan.kind != "RightJoin" && plan.kind != "FullJoin") return rows;
// 内连接和左连接在左表为空时不可能产生结果，可以提前返回。
    const auto right = joinRows(plan.children[1]);
// 再物化右输入，供哈希构建或嵌套循环探测使用。
    if (plan.kind == "HashJoin") {
// 进入哈希连接分支。
        std::size_t leftKey{}, rightKey{};
// 记录连接键在左右输入行中的列号。
        if (!hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey)) fail("HashJoin requires a direct equality key");
// 从谓词中识别直接等值键；哈希连接只支持这种谓词。
        std::unordered_map<std::string, std::vector<std::size_t>> buckets;
// 建立桶表：键的 JSON 文本映射到右表中具有该键的行下标列表。
        for (std::size_t index = 0; index < right.size(); ++index) {
// 第一遍扫描右输入，构建哈希桶。
            checkCancelled();
// 构建过程中同样响应取消。
            if (rightKey >= right[index].size()) fail("HashJoin right key outside row");
// 右表行宽不足说明计划或数据模式不一致，立即报错。
            const auto key = cell(right[index][rightKey]);
// 取出右表这一行的连接键值。
            if (key.is_null()) continue;
// SQL 语义中 NULL 不等于任何值，所以键为 NULL 的行不进入哈希表。
            buckets[key.dump()].push_back(index);
// 用键的稳定文本形式分组，并记录右表行下标以支持一对多连接。
        }
// 哈希表构建结束。
        for (const auto& leftRow : left) {
// 第二遍扫描左输入，探测哈希桶。
            checkCancelled();
// 探测过程中响应取消。
            if (leftKey >= leftRow.size()) fail("HashJoin left key outside row");
// 左表行宽不足同样视为内部错误。
            const auto key = cell(leftRow[leftKey]);
// 取出左表当前行的连接键值。
            if (key.is_null()) continue;
// 左键为 NULL 时不可能匹配，跳过。
            const auto found = buckets.find(key.dump());
// 在哈希桶中查找相同键。
            if (found == buckets.end()) continue;
// 没有匹配的右行时跳过当前左行。
            for (const auto index : found->second) {
// 桶里可能有多行右表数据，逐个与当前左行拼接。
                auto combined = leftRow;
// 复制左行作为连接结果的左半部分。
                combined.insert(combined.end(), right[index].begin(), right[index].end());
// 把匹配到的右行追加到左侧列之后。
                rows.push_back(std::move(combined));
// 把拼接完成的一行加入结果。
            }
// 当前桶内的重复键处理完毕。
        }
// 左输入探测结束。
        return rows;
// 返回哈希连接的全部结果行。
    }
// 哈希连接分支结束，下面处理嵌套循环与各种外连接。
    std::vector<bool> rightMatched(right.size(), false);
// 标记每个右表行是否曾经匹配成功，供右连接和全连接补 NULL 使用。
    for (const auto& a : left) {
// 对左输入逐行进行嵌套循环。
        checkCancelled();
// 每个左行开始前检查取消。
        bool matched = false;
// 记录当前左行是否至少匹配到一个右行。
        for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
// 对右输入逐行尝试连接。
            checkCancelled();
// 内层循环也响应取消。
            const auto& b = right[rightIndex];
// 引用当前右行，避免复制。
            auto combined = a;
// 从当前左行复制出待拼接结果。
            combined.insert(combined.end(), b.begin(), b.end());
// 把当前右行拼接到左行之后。
            if (accepted(evaluate(plan.predicate, combined))) { matched = true;rightMatched[rightIndex] = true;rows.push_back(std::move(combined)); }
// 谓词接受则记录匹配、标记该右行已匹配，并输出这一行。
        }
// 当前左行的右表循环结束。
        if (!matched && (plan.kind == "LeftJoin" || plan.kind == "FullJoin")) {
// 左连接或全连接遇到“没有匹配的右行”时，需要补一行右侧 NULL。
            auto combined = a;
// 复制左行作为结果基础。
            combined.resize(a.size() + plan.children[1].output.size(), std::monostate{});
// 把右侧列宽全部填成 monostate，表示 SQL NULL。
            rows.push_back(std::move(combined));
// 输出这条带 NULL 的补位行。
        }
// 未匹配补位分支结束。
    }
// 左输入遍历结束。
    if (plan.kind == "RightJoin" || plan.kind == "FullJoin") for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) if (!rightMatched[rightIndex]) {
// 右连接或全连接还要输出“没有匹配左行”的右侧数据。
        storage::Row combined(plan.children[0].output.size(), std::monostate{});
// 构造与左输入列数相同的 NULL 行。
        combined.insert(combined.end(), right[rightIndex].begin(), right[rightIndex].end());
// 把未匹配的右行拼到 NULL 左行之后。
        rows.push_back(std::move(combined));
// 输出这条右外连接补位行。
    }
// 未匹配右行扫描结束。
    return rows;
// 返回物化后的连接结果行。
// 返回聚合结果行。
// 返回聚合后的全部行。
// 返回嵌套循环或外连接的结果行。
}
json Database::aggregateRows(const sql::LogicalPlan& plan) {
// 执行聚合节点，返回按分组键聚合后的 JSON 行集合。
    if (plan.children.size() != 1) fail("Aggregate requires one child");
// 聚合只接受一个输入子节点。
    const auto* input = &plan.children.front();
// 从输入子节点开始分析，必要时剥掉外层过滤。
    const json* predicate = nullptr;
// 如果输入是过滤类节点，把谓词单独保存下来，稍后消费行时先筛。
    if (input->kind == "Filter" || input->kind == "SemiJoin" || input->kind == "AntiJoin" || input->kind == "Apply") {
// 过滤、半连接、反连接和 Apply 都可以直接下推到聚合前。
        if (input->children.size() != 1) fail("Aggregate filter requires one child");
// 过滤类输入同样只能有一个子节点。
        predicate = &input->predicate;
// 保存过滤谓词指针；它指向计划树中的对象，生命周期覆盖本次执行。
        input = &input->children.front();
// 真正扫描的数据源是过滤节点的孩子。
    }
// 过滤剥离结束。
    json initial = json::array();
// 每个聚合函数都有一个初始状态，先把它们组装成数组。
    for (const auto& function : plan.aggregates)
// 逐个聚合函数生成初始状态。
        initial.push_back(function.at("function") == "COUNT" ? json(std::int64_t{0}) :
// COUNT 从 0 开始。
            function.at("function") == "AVG" ? (function.at("argument").at("type") == "float" ?
// AVG 需要同时记录和与计数；浮点和定点数的表示不同。
                json{{"sum", 0.0}, {"count", std::int64_t{0}}} : json{{"sum", "0"}, {"count", std::int64_t{0}}}) : json(nullptr));
// 定点数用字符串保存“和”，避免整数溢出或精度丢失。

    const auto combine = [&](json& state, const json& aggregate, const json& value, SourceLocation location) {
// 定义单个聚合函数的增量合并逻辑：state 是当前状态，value 是新值。
        if (value.is_null()) return;
// SQL 聚合函数忽略 NULL 值，所以空值直接返回。
        const auto function = aggregate.at("function").get<std::string>();
// 取出函数名，决定走哪条累加路径。
        const auto& argument = aggregate.at("argument");
// 取出参数表达式，后面要用它的类型和位置。
        json next = state;
// 复制当前状态，先在新对象上计算，最后再整体替换。
        if (function == "COUNT") next = arithmetic64("+", state.get<std::int64_t>(), 1, location);
// COUNT：每遇到一个非空值就把计数加一，并检查 64 位溢出。
        else if (function == "SUM") {
// SUM：根据参数类型分别处理定点数、浮点数和整数。
            if (const auto type = decimalType(argument.at("type").get<std::string>())) {
// 先看参数是不是 DECIMAL 等定点类型。
                next = state.is_null() ? value : json(ExactDecimal::parse(state.get<std::string>(), 38, type->scale).arithmetic("+", decimalValue(value, type->name()), location).format());
// 定点求和：首次赋值直接取 value，之后用 ExactDecimal 做精确加法。
            } else if (argument.at("type") == "float")
// 其次处理 float 参数。
                next = state.is_null() ? value : json(requireFiniteFloat(state.get<double>() + value.get<double>(), location));
// 浮点求和后要检查有限性，避免 inf/NaN 进入结果。
            else next = state.is_null() ? value : json(arithmetic64("+", state.get<std::int64_t>(), value.get<std::int64_t>(), location));
// 剩下的整数类型用 arithmetic64 求和，同样做溢出检查。
        } else if (function == "MIN" || function == "MAX") {
// MIN/MAX：只要保存当前最小或最大值即可。
            if (next.is_null()) next = value;
// 第一次遇到非空值时直接初始化。
            else {
// 否则与当前值比较。
                const auto type = argument.at("type").get<std::string>();
// 取出参数类型名。
                const auto order = decimalType(type) ? decimalValue(value, type).compare(decimalValue(next, type)) : value < next ? -1 : value > next ? 1 : 0;
// 定点数走 decimalValue.compare，其他类型直接用 JSON 的小于/大于比较。
                if ((function == "MIN" && order < 0) || (function == "MAX" && order > 0)) next = value;
// MIN 遇到更小值、MAX 遇到更大值时替换。
            }
// MIN/MAX 合并逻辑结束。
        } else if (function == "AVG") {
// AVG：需要维护和与计数两个分量。
            if (argument.at("type") == "float") {
// 浮点 AVG 分支。
                next = {{"sum", requireFiniteFloat(state.at("sum").get<double>() + value.get<double>(), location)},
// 累加浮点和，并检查有限性。
                    {"count", arithmetic64("+", state.at("count").get<std::int64_t>(), 1, location)}};
// 同时把计数加一。
            } else {
// 定点/整数 AVG 分支。
                ExactDecimal::Integer total(state.at("sum").get<std::string>());
// 用大整数保存当前总和，避免直接浮点求和带来精度损失。
                if (decimalType(argument.at("type").get<std::string>())) total += decimalValue(value, argument.at("type").get<std::string>()).coefficient();
// 定点参数先转成统一 scale 的系数再累加。
                else total += value.get<std::int64_t>();
// 整数参数直接累加到总和中。
                next = {{"sum", total.convert_to<std::string>()},
// 更新总和字符串。
                    {"count", arithmetic64("+", state.at("count").get<std::int64_t>(), 1, location)}};
// 更新计数。
            }
// AVG 合并结束。
        } else throw MiniSqlError(ErrorCode::NotImplemented, "Aggregate function evaluation is not implemented: " + function);
// 未知聚合函数属于未实现功能，直接报错。
        state = std::move(next);
// 把计算好的新状态放回原位置。
    };
// combine lambda 结束。

    std::vector<json> records;
// records 保存每一行的分组键与各聚合函数输入值，后面排序后同组相邻。
    const auto consume = [&](const storage::Row& row) {
// 定义消费一行的逻辑：先做过滤，再把分组键和聚合参数算出来。
        checkCancelled();
// 每消费一行前检查取消。
        if (predicate && !accepted(evaluate(*predicate, row))) return;
// 有外层过滤谓词时先求值；不接受就丢弃这一行。
        json groupKey = json::array();
// 分组键是表达式数组，可能包含多列。
        for (const auto& expression : plan.groupKeys) groupKey.push_back(evaluate(expression, row));
// 对每个 GROUP BY 表达式求值，保持列顺序。
        json values = json::array();
// 保存本行传给各聚合函数的参数值。
        for (const auto& aggregate : plan.aggregates) {
// 遍历计划中的聚合函数。
            const auto& argument = aggregate.at("argument");
// 取出当前聚合函数的参数表达式。
            values.push_back(argument.is_null() ? json(true) : evaluate(argument, row));
// 无参数聚合（如 COUNT(*)）用 true 占位；有参数则现场求值。
        }
// 聚合参数遍历结束。
        records.push_back({{"key", std::move(groupKey)}, {"values", std::move(values)}});
// 把分组键和参数值组成一条记录放入 records。
    };
// consume lambda 结束。
    if (input->kind == "SeqScan") {
// 输入是顺序扫描：直接把堆表行喂给 consume。
        const catalog::StoredTable* table = nullptr;
// 先按表名找到表定义。
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(input->table)) table = &candidate;
// 在目录中做大小写不敏感匹配。
        if (!table) fail("Aggregate scan references missing table");
// 表不存在说明计划与目录不一致，立即失败。
        heap_.scan(table->id, rowSchema(table->definition), [&](storage::RowRef, const storage::Row& row) { consume(row); });
// 用堆表扫描器逐行读取并消费。
    } else if (input->kind == "NestedLoopJoin" || input->kind == "HashJoin" || input->kind == "LeftJoin" || input->kind == "RightJoin" || input->kind == "FullJoin") {
// 输入是各种 JOIN：先物化连接结果，再逐行消费。
        for (const auto& row : joinRows(*input)) consume(row);
// joinRows 返回全部连接行，逐行交给 consume。
    } else if (input->kind == "Limit" && input->limit && *input->limit == 0) {
// Limit 0 表示输入恒为空。
        // 恒假过滤已被改写为 Limit 0；空输入仍需保留全局聚合的一行结果。
        // 恒假过滤已被改写为 Limit 0；空输入仍需保留全局聚合的一行结果。
    } else fail("Unsupported aggregate input");
// 其他计划类型不支持直接聚合，属于内部错误。
    if (plan.groupKeys.empty() && records.empty()) {
// 没有 GROUP BY 且输入为空时，SQL 仍要求输出一行聚合结果（COUNT=0，其余 NULL）。
        json values = json::array();
// 构造这一行的聚合值数组。
        for (std::size_t index = 0; index < plan.aggregates.size(); ++index) values.push_back(nullptr);
// 每个聚合函数先填 NULL 占位。
        records.push_back({{"key", json::array()}, {"values", std::move(values)}});
// 插入唯一一条空分组记录。
    }
// 空输入补行逻辑结束。

    const bool external = records.size() > aggregateMemoryRows_;
// 记录数超过内存阈值时改用外排，避免聚合记录把内存撑爆。
    const auto compareRecords = [](const json& left, const json& right) { return left.at("key") < right.at("key"); };
// 按分组键排序，使同组记录在序列中相邻。
    if (external) {
// 外排分支。
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-a" + std::to_string(++aggregateSequence_);
// 生成本次聚合排序的唯一操作号，用于临时文件命名。
        externalSort(records, compareRecords, aggregateMemoryRows_, sortTempDirectory_, operationId, [this] { checkCancelled(); });
// 调用 externalSort，并传入取消检查回调。
    } else std::stable_sort(records.begin(), records.end(), compareRecords);
// 记录数在内存预算内时直接用稳定排序。

    constexpr std::size_t maximumPayload = 64 * 1024 * 1024;
// 单次聚合结果允许占用的最大内存预算：64 MiB。
    json rows = json::array();
// 最终输出的行集合。
    json currentKey = json::array();
// 当前正在累计的分组键。
    json state = initial;
// 当前分组各聚合函数的累计状态。
    bool active = false;
// 是否已经进入某个分组。
    std::size_t groupCount = 0, groupPayloadBytes = 0, totalPayloadBytes = 0;
// 分组数、当前分组状态大小、全部状态总大小，用于预算控制。
    const auto emit = [&]() {
// 定义 emit：把当前分组状态转换成一行输出。
        auto row = currentKey;
// 输出行先放入分组键列。
        for (std::size_t index = 0; index < state.size(); ++index) {
// 逐个聚合函数追加结果列。
            if (plan.aggregates[index].at("function") != "AVG") row.push_back(state[index]);
// 不是 AVG 的函数直接输出累计状态。
            else {
// AVG 需要把“和”除以计数后再输出。
                const auto count = state[index].at("count").get<std::int64_t>();
// 取出 AVG 的计数。
                if (plan.aggregates[index].at("type") == "float") {
// 浮点 AVG 分支。
                    const auto& aggregateArgument = plan.aggregates[index].at("argument");
// 找到参数表达式以获取源码位置，便于除零等错误报行号列号。
                    row.push_back(count == 0 ? json(nullptr) : json(requireFiniteFloat(state[index].at("sum").get<double>() / static_cast<double>(count), {aggregateArgument.value("line", std::size_t{0}), aggregateArgument.value("column", std::size_t{0})})));
// 计数为 0 时输出 NULL；否则计算平均值并检查有限性。
                    continue;
// 当前聚合列处理完，继续下一列。
                }
// 浮点 AVG 分支结束。
                ExactDecimal::Integer denominator = count;
// 定点/整数 AVG：先把计数当作分母。
                const auto argumentType = decimalType(plan.aggregates[index].at("argument").at("type").get<std::string>());
// 查看参数是否为定点类型，以便把 scale 还原到分母。
                if (argumentType) for (unsigned digit = 0; digit < argumentType->scale; ++digit) denominator *= 10;
// 定点数的和在累加时可能带 scale，分母也要乘以 10 的 scale 次方。
                const auto outputType = decimalType(plan.aggregates[index].at("type").get<std::string>());
// 取出输出列的定点类型，决定结果 scale。
                try {
// 做精确除法，可能因为精度或范围失败。
                    row.push_back(count == 0 ? json(nullptr) : json(ExactDecimal::fromRatio(
// 计数为 0 输出 NULL；否则用 ExactDecimal::fromRatio 做精确除法。
                        ExactDecimal::Integer(state[index].at("sum").get<std::string>()), denominator, 38, outputType->scale).format()));
// 把精确除法结果格式化成数据库输出的字符串。
                } catch (const MiniSqlError& error) {
// 捕获精确除法失败，补充 SQL 源码位置后重新抛出。
                    const auto& argument = plan.aggregates[index].at("argument");
// 重新取参数位置。
                    throw MiniSqlError(error.code(), error.what(), {argument.value("line", std::size_t{0}), argument.value("column", std::size_t{0})});
// 抛出带行号列号的错误，方便定位是哪一列聚合出错。
                }
// 定点 AVG 异常处理结束。
            }
// AVG 分支结束。
        }
// 所有聚合列处理结束。
        if (row.size() != plan.output.size()) fail("Aggregate output schema mismatch");
// 输出列数必须与计划声明的模式一致。
        rows.push_back(std::move(row));
// 把当前分组结果放入最终行集合。
    };
// emit lambda 结束。

    for (const auto& record : records) {
// 按排序后的顺序扫描所有聚合记录，同键记录相邻。
        if (!record.is_object() || !record.contains("key") || !record.contains("values") || !record.at("key").is_array() ||
// 校验记录结构必须包含合法的 key 与 values。
            !record.at("values").is_array() || record.at("values").size() != plan.aggregates.size())
// values 必须是数组且长度等于聚合函数个数。
            fail("Aggregate record schema mismatch");
// 结构不合法说明执行器内部状态错误。
        if (!active || record.at("key") != currentKey) {
// 第一次进入或分组键变化时，说明上一组结束、新组开始。
            if (active) emit();
// 上一组已经累计过，先输出它。
            if (++groupCount > 65536) fail("Aggregate group budget exceeded");
// 分组数超过硬上限时拒绝继续，避免恶意分组耗尽资源。
            currentKey = record.at("key");
// 切换当前分组键。
            state = initial;
// 新分组的聚合状态恢复为初始值。
            active = true;
// 标记已经进入分组。
            groupPayloadBytes = currentKey.dump().size() + state.dump().size();
// 计算新分组键和初始状态的序列化大小。
            if (!external) {
// 内存模式才需要跟踪全局状态预算。
                if (groupPayloadBytes > maximumPayload - totalPayloadBytes) fail("Aggregate state payload budget exceeded");
// 新分组会超过总预算时立即报错。
                totalPayloadBytes += groupPayloadBytes;
// 把新分组大小计入总量。
            }
// 预算控制分支结束。
        }
// 新分组初始化结束。
        const auto& values = record.at("values");
// 取出本行的聚合参数值。
        for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
// 逐个聚合函数更新状态。
            if (values[index].is_null()) continue;
// NULL 参数不参与聚合，直接跳过。
            const auto& aggregate = plan.aggregates[index];
// 取出对应聚合函数定义。
            const auto& argument = aggregate.at("argument");
// 取出参数表达式。
            const SourceLocation location{argument.is_null() ? 0 : argument.value("line", std::size_t{0}),
// 读取参数的行号。
                argument.is_null() ? 0 : argument.value("column", std::size_t{0})};
// 读取参数的列号；无参数聚合则记为 0。
            const auto oldSize = state[index].dump().size();
// 记录更新前的状态大小，后面按增量更新预算。
            combine(state[index], aggregate, values[index], location);
// 调用 combine 把当前值并入聚合状态。
            const auto newSize = state[index].dump().size();
// 计算更新后的状态大小。
            if (newSize > oldSize && newSize - oldSize > maximumPayload - groupPayloadBytes) fail("Aggregate state payload budget exceeded");
// 状态增长超过分组预算时立即失败。
            groupPayloadBytes = groupPayloadBytes - oldSize + newSize;
// 用增量更新当前分组的状态大小。
            if (!external) totalPayloadBytes = totalPayloadBytes - oldSize + newSize;
// 内存模式下同步更新全局状态大小。
        }
// 当前记录的聚合列更新结束。
    }
// 所有聚合记录扫描结束。
    if (active) emit();
// 还有最后一个分组未输出时补一次 emit。
    return rows;
// 返回聚合后的全部行。
}
std::unique_ptr<RowStream> Database::scanRowStream(const sql::LogicalPlan& plan) {
// 为顺序扫描创建流式行读取器，避免一次性物化整表。
    const catalog::StoredTable* table = nullptr;
// 先定位计划引用的表定义。
    for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
// 在目录中按表名做大小写不敏感查找。
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Plan references missing table");
// 表缺失说明计划与目录不一致，报目录错误。
    return std::make_unique<ScanRowStream>(heap_, table->id, rowSchema(table->definition));
// 用堆表扫描器构造 ScanRowStream，每次 next 只取一行。
}

std::unique_ptr<RowStream> Database::openRowStream(const sql::LogicalPlan& plan) {
// 按计划节点类型创建对应的行流，支持流式执行算子。
    checkCancelled();
// 创建任何行流前先检查取消。
    if (!activeResources_) activeResources_ = std::make_shared<QueryResourceManager>(queryMemoryBytes_, tempDiskBytes_);
    if (plan.kind == "SeqScan" && !plan.orderedScan) return scanRowStream(plan);
    // 普通顺序扫描走原路径；带有序标记的 SeqScan 交给下面的有序分支处理。
    if (plan.kind == "SeqScan" && plan.orderedScan) {
    // 有序索引扫描：按索引键升序取出全部行引用，交给行流按序输出。
    // 上层因此可以省掉排序算子——这正是本优化的收益所在。
        const catalog::StoredTable* table = nullptr;
        // 定位表定义。
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
        // 按表名做大小写不敏感查找。
        if (!table) fail("Ordered scan references missing table");
        // 表不存在属于计划与目录不一致。
        const RuntimeIndex* index = nullptr;
        // 找到计划指定的运行期索引。
        for (const auto& candidate : indexes_)
            if (key(candidate->table) == key(plan.table) && key(candidate->name) == key(plan.indexName)) index = candidate.get();
        if (!index) fail("Ordered scan references missing runtime index");
        // 索引缺失说明未打开或计划过期。
        auto refs = index->range(std::nullopt, true, std::nullopt, true);
        // 无界范围查询即「按索引键顺序遍历全索引」；B+ 树叶链天然有序。
        if (plan.orderedDescending) std::reverse(refs.begin(), refs.end());
        // 降序只需把有序结果整体反转，无需重新排序。
        return std::make_unique<ScanRowStream>(heap_, table->id, rowSchema(table->definition), std::move(refs));
        // 复用现成的按引用列表输出的行流。
    }
    // 有序扫描分支结束。
// 顺序扫描直接交给 scanRowStream。
    if (plan.kind == "IndexScan") {
        const catalog::StoredTable* table = nullptr;
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
        if (!table) fail("IndexScan references missing table");
        const RuntimeIndex* index = nullptr;
        for (const auto& candidate : indexes_)
            if (key(candidate->table) == key(plan.table) && key(candidate->name) == key(plan.indexName)) index = candidate.get();
        if (!index) fail("IndexScan references missing runtime index");
        std::vector<storage::RowRef> refs;
        if (!plan.indexRangeOperator.empty()) {
            storage::IndexKey searchKey;
            for (const auto& value : plan.indexValues)
                searchKey.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
            searchKey.values.push_back(indexValue(plan.indexRangeValue.at("value"), plan.indexRangeValue.at("type").get<std::string>()));
            const auto& op = plan.indexRangeOperator;
            if (op == ">") refs = index->range(searchKey, false, std::nullopt, true);
            else if (op == ">=") refs = index->range(searchKey, true, std::nullopt, true);
            else if (op == "<") refs = index->range(std::nullopt, true, searchKey, false);
            else refs = index->range(std::nullopt, true, searchKey, true);
        } else if (!plan.indexValues.empty()) {
            storage::IndexKey searchKey;
            for (const auto& value : plan.indexValues)
                searchKey.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
            refs = index->search(searchKey);
        } else {
            const auto& predicate = plan.predicate;
            if (!predicate.is_object()) fail("IndexScan requires a predicate");
            const auto op = predicate.value("operator", "");
            const auto& right = predicate.at("right");
            if (!right.is_object() || right.value("kind", "") != "Literal") fail("IndexScan requires a literal key");
            const auto searchKey = storage::IndexKey{{indexValue(right.at("value"), right.at("type").get<std::string>())}};
            if (op == "=") refs = index->search(searchKey);
            else if (op == ">") refs = index->range(searchKey, false, std::nullopt, true);
            else if (op == ">=") refs = index->range(searchKey, true, std::nullopt, true);
            else if (op == "<") refs = index->range(std::nullopt, true, searchKey, false);
            else if (op == "<=") refs = index->range(std::nullopt, true, searchKey, true);
            else fail("IndexScan requires an indexed comparison");
        }
        return std::make_unique<ScanRowStream>(heap_, table->id, rowSchema(table->definition), std::move(refs));
    }
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
// 过滤、半连接、反连接和 Apply 都包装一个子流并施加谓词。
        if (plan.children.size() != 1) fail("Filter requires one child");
// 过滤类节点必须恰好有一个孩子。
        auto child = openRowStream(plan.children.front());
// 递归创建子节点的行流。
        const auto predicate = plan.predicate;
// 复制谓词，保证 lambda 在流生命周期内持有稳定对象。
        return std::make_unique<FilterRowStream>(std::move(child), [this, predicate](const json& row) {
// 用子行流和过滤谓词构造 FilterRowStream。
            return accepted(evaluate(predicate, row));
// lambda 对每一行求谓词，只保留 accepted 为真的行。
        });
// FilterRowStream 构造结束。
    }
// 过滤分支结束。
    if (plan.kind == "Project") {
// Project 节点负责表达式投影或按列编号取列。
        if (plan.children.size() != 1) fail("Project requires one child");
// 投影必须恰好有一个输入。
        auto child = openRowStream(plan.children.front());
// 创建输入行流。
        const auto projections = plan.projections;
// 复制投影表达式列表。
        const auto output = plan.output;
// 复制输出列模式，用于无显式表达式时按列号取列。
        return std::make_unique<ProjectRowStream>(std::move(child), [this, projections, output](const json& row) {
// 构造 ProjectRowStream，并传入逐行投影函数。
            json projected = json::array();
// 准备投影后的行数组。
            if (!projections.empty()) {
// 有显式投影表达式时逐个求值。
                for (const auto& expression : projections) projected.push_back(evaluate(expression, row));
// 依次执行投影表达式并追加结果列。
            } else {
// 没有显式表达式时按输出模式的列号取列。
                for (const auto& column : output) {
// 遍历输出模式的每一列。
                    if (column.columnId >= row.size()) fail("Projection outside row");
// 列号越界说明计划与输入模式不一致。
                    projected.push_back(cell(row.at(column.columnId)));
// 从输入行取出对应单元格并追加。
                }
// 输出列遍历结束。
            }
// 无显式投影分支结束。
            return projected;
// 返回投影后的行。
        });
// ProjectRowStream 构造结束。
    }
// Project 分支结束。
    if (plan.kind == "Limit") {
// Limit 节点：流式跳过 offset 并限制输出行数。
// Limit 节点优先走流式路径。
// Limit 节点优先走流式路径：不物化整表，只取 offset 之后需要的前若干行。
// Limit 节点用流式方式跳过 offset 并限制返回行数。
        if (plan.children.size() != 1) fail("Limit requires one child");
// Limit 必须恰好有一个输入。
// Limit 必须恰好有一个输入。
// Limit 必须恰好有一个输入。
// Limit 必须恰好有一个输入。
        auto child = openRowStream(plan.children.front());
// 创建输入行流。
        return std::make_unique<LimitRowStream>(std::move(child), plan.offset, plan.limit);
// 构造 LimitRowStream，传入偏移和上限。
    }
// 该语句分支结束。
// Limit 分支结束。
    if (plan.kind == "Sort") {
        if (plan.children.size() != 1) fail("Sort requires one child");
        auto child = openRowStream(plan.children.front());
        const auto sortKeys = plan.sortKeys;
        const auto inputSchema = plan.children.front().output;
        const auto compareRows = [sortKeys, inputSchema](const json& left, const json& right) {
            for (const auto& sort : sortKeys) {
                const auto index = sort.at("index").get<std::size_t>();
                const auto& a = left.at(index);
                const auto& b = right.at(index);
                if (a == b) continue;
                if (a.is_null() || b.is_null()) return a.is_null() ? sort.at("nullsFirst").get<bool>() : !sort.at("nullsFirst").get<bool>();
                if (index < inputSchema.size() && decimalType(inputSchema[index].type)) {
                    const auto& type = inputSchema[index].type;
                    const auto order = decimalValue(a, type).compare(decimalValue(b, type));
                    if (order == 0) continue;
                    return sort.at("descending").get<bool>() ? order > 0 : order < 0;
                }
                return sort.at("descending").get<bool>() ? a > b : a < b;
            }
            return false;
        };
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-s" + std::to_string(++sortSequence_);
        return std::make_unique<ExternalSortRowStream>(std::move(child), compareRows, activeResources_,
            sortTempDirectory_, operationId, sortMemoryRows_, [this] { checkCancelled(); }, plan.output.size(),
            // Top-N：优化器把上方 Limit 的条数下推到本节点；有值时只保留前 N 行。
            plan.limit ? std::optional<std::size_t>(static_cast<std::size_t>(*plan.limit)) : std::nullopt);
    }
    if (plan.kind == "Distinct") {
        if (plan.children.size() != 1) fail("Distinct requires one child");
        auto child = openRowStream(plan.children.front());
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-d" + std::to_string(++sortSequence_);
        auto sorted = std::make_unique<ExternalSortRowStream>(std::move(child),
            [](const json& left, const json& right) { return left < right; }, activeResources_,
            sortTempDirectory_, operationId, distinctMemoryRows_, [this] { checkCancelled(); });
        return std::make_unique<DistinctRowStream>(std::move(sorted), activeResources_);
    }
    if (plan.kind == "Aggregate") {
        if (plan.children.size() != 1) fail("Aggregate requires one child");
        auto child = openRowStream(plan.children.front());
        const auto groupKeys = plan.groupKeys;
        const auto aggregates = plan.aggregates;
        json initial = json::array();
        json emptyValues = json::array();
        for (const auto& aggregate : aggregates) {
            initial.push_back(aggregate.at("function") == "COUNT" ? json(std::int64_t{0}) :
                aggregate.at("function") == "AVG" ? (aggregate.at("argument").at("type") == "float" ?
                    json{{"sum", 0.0}, {"count", std::int64_t{0}}} : json{{"sum", "0"}, {"count", std::int64_t{0}}}) : json(nullptr));
            emptyValues.push_back(nullptr);
        }
        std::optional<json> emptyRecord;
        if (groupKeys.empty()) emptyRecord = json{{"key", json::array()}, {"values", emptyValues}};
        auto records = std::make_unique<MappingRowStream>(std::move(child),
            [this, groupKeys, aggregates](const json& input) {
                json groupKey = json::array(), values = json::array();
                for (const auto& expression : groupKeys) groupKey.push_back(evaluate(expression, input));
                for (const auto& aggregate : aggregates) {
                    const auto& argument = aggregate.at("argument");
                    values.push_back(argument.is_null() ? json(true) : evaluate(argument, input));
                }
                return json{{"key", std::move(groupKey)}, {"values", std::move(values)}};
            }, emptyRecord);
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-a" + std::to_string(++aggregateSequence_);
        auto sorted = std::make_unique<ExternalSortRowStream>(std::move(records),
            [](const json& left, const json& right) { return left.at("key") < right.at("key"); },
            activeResources_, sortTempDirectory_, operationId, aggregateMemoryRows_, [this] { checkCancelled(); });
        const auto combine = [aggregates](json& state, const json& values) {
            if (!values.is_array() || values.size() != aggregates.size()) fail("Aggregate record schema mismatch");
            for (std::size_t index = 0; index < aggregates.size(); ++index) {
                const auto& aggregate = aggregates[index];
                const auto& value = values[index];
                if (value.is_null()) continue;
                const auto function = aggregate.at("function").get<std::string>();
                const auto& argument = aggregate.at("argument");
                const SourceLocation location{
                    argument.is_object() ? argument.value("line", std::size_t{0}) : 0,
                    argument.is_object() ? argument.value("column", std::size_t{0}) : 0};
                json next = state[index];
                if (function == "COUNT") next = arithmetic64("+", state[index].get<std::int64_t>(), 1, location);
                else if (function == "SUM") {
                    if (const auto type = decimalType(argument.at("type").get<std::string>()))
                        next = state[index].is_null() ? value : json(ExactDecimal::parse(state[index].get<std::string>(), 38, type->scale)
                            .arithmetic("+", decimalValue(value, type->name()), location).format());
                    else if (argument.at("type") == "float")
                        next = state[index].is_null() ? value : json(requireFiniteFloat(state[index].get<double>() + value.get<double>(), location));
                    else next = state[index].is_null() ? value : json(arithmetic64("+", state[index].get<std::int64_t>(), value.get<std::int64_t>(), location));
                } else if (function == "MIN" || function == "MAX") {
                    if (next.is_null()) next = value;
                    else {
                        const auto type = argument.at("type").get<std::string>();
                        const auto order = decimalType(type) ? decimalValue(value, type).compare(decimalValue(next, type)) : value < next ? -1 : value > next ? 1 : 0;
                        if ((function == "MIN" && order < 0) || (function == "MAX" && order > 0)) next = value;
                    }
                } else if (function == "AVG") {
                    if (argument.at("type") == "float")
                        next = {{"sum", requireFiniteFloat(state[index].at("sum").get<double>() + value.get<double>(), location)},
                            {"count", arithmetic64("+", state[index].at("count").get<std::int64_t>(), 1, location)}};
                    else {
                        ExactDecimal::Integer total(state[index].at("sum").get<std::string>());
                        if (decimalType(argument.at("type").get<std::string>())) total += decimalValue(value, argument.at("type").get<std::string>()).coefficient();
                        else total += value.get<std::int64_t>();
                        next = {{"sum", total.convert_to<std::string>()},
                            {"count", arithmetic64("+", state[index].at("count").get<std::int64_t>(), 1, location)}};
                    }
                } else throw MiniSqlError(ErrorCode::NotImplemented, "Aggregate function evaluation is not implemented: " + function);
                state[index] = std::move(next);
            }
        };
        const auto outputSize = plan.output.size();
        const auto finalize = [aggregates, outputSize](const json& groupKey, const json& state) {
            json row = groupKey;
            for (std::size_t index = 0; index < state.size(); ++index) {
                if (aggregates[index].at("function") != "AVG") { row.push_back(state[index]); continue; }
                const auto count = state[index].at("count").get<std::int64_t>();
                const auto& argument = aggregates[index].at("argument");
                const SourceLocation location{argument.value("line", std::size_t{0}), argument.value("column", std::size_t{0})};
                if (aggregates[index].at("type") == "float") {
                    row.push_back(count == 0 ? json(nullptr) : json(requireFiniteFloat(
                        state[index].at("sum").get<double>() / static_cast<double>(count), location)));
                    continue;
                }
                ExactDecimal::Integer denominator = count;
                const auto argumentType = decimalType(argument.at("type").get<std::string>());
                if (argumentType) for (unsigned digit = 0; digit < argumentType->scale; ++digit) denominator *= 10;
                const auto outputType = decimalType(aggregates[index].at("type").get<std::string>());
                try {
                    row.push_back(count == 0 ? json(nullptr) : json(ExactDecimal::fromRatio(
                        ExactDecimal::Integer(state[index].at("sum").get<std::string>()), denominator, 38, outputType->scale).format()));
                } catch (const MiniSqlError& error) {
                    throw MiniSqlError(error.code(), error.what(), location);
                }
            }
            if (row.size() != outputSize) fail("Aggregate output schema mismatch");
            return row;
        };
        return std::make_unique<GroupedAggregateRowStream>(std::move(sorted), std::move(initial), combine, finalize, activeResources_);
    }
    if (plan.kind == "NestedLoopJoin" || plan.kind == "HashJoin" || plan.kind == "LeftJoin") {
        if (plan.children.size() != 2) fail("Join requires two children");
        auto left = openRowStream(plan.children[0]);
        auto right = openRowStream(plan.children[1]);
        std::optional<std::pair<std::size_t, std::size_t>> keys;
        if (plan.kind == "HashJoin") {
            std::size_t leftKey = 0, rightKey = 0;
            if (!hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey))
                fail("HashJoin requires a direct equality key");
            keys = std::pair{leftKey, rightKey};
        }
        const auto predicate = plan.predicate;
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-j" + std::to_string(++joinSequence_);
        return std::make_unique<JoinRowStream>(std::move(left), std::move(right),
            [this, predicate](const json& row) { return accepted(evaluate(predicate, row)); },
            activeResources_, sortTempDirectory_, operationId, joinMemoryRows_, plan.children[1].output.size(),
            plan.kind == "LeftJoin", keys, [this] { checkCancelled(); });
    }
// 物化类分支结束。
    throw MiniSqlError(ErrorCode::InvalidArgument, "RowStream does not support plan kind " + plan.kind);
// 其他计划类型没有行流实现，属于参数错误。
// 行流不支持该计划类型，抛出参数错误。
}

nlohmann::json Database::runNode(const sql::LogicalPlan& plan) {
// 执行单个非扫描计划节点，返回统一的 JSON 结果结构。
    checkCancelled();
// 执行前检查取消标志。
    if (plan.kind == "Sort") {
// Sort 分支：先执行孩子，再对结果排序。
        try {
            auto stream = openRowStream(plan);
            json rows = json::array(), row;
            while (stream->next(row)) rows.push_back(std::move(row));
            stream->close();
            auto usage = stream->resourceUsage();
            json columns = json::array();
            for (const auto& column : plan.output) columns.push_back(column.name);
            return {{"kind", "Sort"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
                {"affectedRows", 0}, {"resourceUsage", std::move(usage)}};
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::InvalidArgument) throw;
        }
        if (plan.children.size() != 1) fail("Sort requires one child");
// 排序必须恰好有一个输入。
        auto result = run(plan.children.front());
// 递归执行子节点，得到待排序结果。
        auto& rows = result["rows"];
// 取得结果中的行数组引用，后面原地排序。
        const auto compareRows = [&](const json& left, const json& right) {
// 定义比较器：按 sortKeys 逐个比较。
            for (const auto& sort : plan.sortKeys) {
// 遍历排序键，前面的键优先。
                const auto index = sort.at("index").get<std::size_t>();
// 取出当前排序键对应的列下标。
                const auto& a = left.at(index);
// 取出左行的比较值。
                const auto& b = right.at(index);
// 取出右行的比较值。
                if (a == b) continue;
// 当前键相等时继续比较下一个键。
                if (a.is_null() || b.is_null()) return a.is_null() ? sort.at("nullsFirst").get<bool>() : !sort.at("nullsFirst").get<bool>();
// 处理 NULL 排序；nullsFirst 决定 NULL 排前还是排后。
                if (decimalType(plan.children.front().output.at(index).type)) {
// 定点类型不能直接用 JSON 数值比较，需要按 decimal 规则比较。
                    const auto& type = plan.children.front().output.at(index).type;
// 取出该列的定点类型定义。
                    const auto order = decimalValue(a, type).compare(decimalValue(b, type));
// 用 decimalValue.compare 得到 -1/0/1。
                    if (order == 0) continue;
// 相等则继续下一个排序键。
                    return sort.at("descending").get<bool>() ? order > 0 : order < 0;
// 按 descending 决定正序还是逆序返回。
                }
// 定点比较分支结束。
                return sort.at("descending").get<bool>() ? a > b : a < b;
// 其他类型直接使用 JSON 的运算符比较，并按 descending 取反。
            }
// 列名建议分支结束。
// 所有排序键都比较完仍未分出大小。
            return false;
// 两行在排序语义下相等。
        };
// compareRows lambda 结束。
        const bool external = rows.size() > sortMemoryRows_;
        if (external) {
            const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-s" + std::to_string(++sortSequence_);
// 生成本次排序操作的唯一标识，用于临时文件命名。
            externalSort(rows, compareRows, sortMemoryRows_, sortTempDirectory_, operationId, [this] { checkCancelled(); });
// 调用 externalSort 完成多路归并排序。
        } else std::stable_sort(rows.begin(), rows.end(), compareRows);
        for (auto& row : rows) while (row.size() > plan.output.size()) row.erase(row.end() - 1);
// 排序可能产生多余尾列，按输出模式裁掉。
        result["columns"] = json::array();
// 重建结果列名数组。
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 按输出模式填入列名。
        result["kind"] = "Sort";
// 标记结果来自 Sort 节点。
        result["resourceUsage"] = {{"kind", "Sort"}, {"rows", rows.size()},
// 记录排序资源使用情况，便于诊断接口展示。
            {"external", external}, {"memoryRows", sortMemoryRows_}};
        return result;
// 返回排序后的结果。
    }
// Sort 分支结束。
    if (plan.kind == "Limit") {
// Limit 节点：跳过 offset 并限制输出行数。
        if (plan.children.size() != 1) fail("Limit requires one child");
// Limit 必须恰好有一个输入。
        if (plan.limit && *plan.limit == 0) {
// LIMIT 0 是恒空查询，可以直接返回空结果。
            json columns = json::array();
// 空结果仍然需要按输出模式给出列名。
            for (const auto& column : plan.output) columns.push_back(column.name);
// 逐个复制输出列名。
            return {{"kind", "Limit"}, {"columns", columns}, {"rows", json::array()}, {"affectedRows", 0}};
// 返回零行结果，kind 标记为 Limit。
        }
// 非零 LIMIT 进入流式读取。
        try {
// 流式路径可能因为下层不支持行流而失败，用 try 捕获后回退物化。
            auto stream = openRowStream(plan.children.front());
// 为子计划创建行流。
            json rows = json::array();
// 保存最终要返回的行。
            json row;
// 复用一个 JSON 对象接收每次 next 读出的行。
            for (std::uint64_t skipped = 0; skipped < plan.offset; ++skipped) {
// 逐个跳过 offset 指定的行。
                if (!stream->next(row)) break;
// 流已经结束就停止跳过。
            }
// offset 跳过结束。
            std::uint64_t emitted = 0;
// 记录已经输出的行数。
            while (!plan.limit || emitted < *plan.limit) {
// 只要没有上限或还没达到上限，就继续取行。
                if (!stream->next(row)) break;
// 流结束则退出。
                rows.push_back(std::move(row));
// 把当前行移入结果集。
                ++emitted;
// 输出计数加一。
            }
// 取行循环结束。
            stream->close();
// 显式关闭行流，释放扫描器持有的资源。
            auto childUsage = stream->resourceUsage();
            json columns = json::array();
// 构造输出列名数组。
            for (const auto& column : plan.output) columns.push_back(column.name);
// 按计划输出模式复制列名。
            return {{"kind", "Limit"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
// 返回流式 Limit 结果。
                {"affectedRows", 0}, {"resourceUsage", {{"kind", "LimitRowStream"}, {"rows", emitted},
                    {"child", std::move(childUsage)}}}};
        } catch (const MiniSqlError& error) {
// 捕获下层不支持行流的错误。
            if (error.code() != ErrorCode::InvalidArgument) throw;
// 只有 InvalidArgument 才回退物化；其他错误继续抛出。
        }
// try 块结束，下面走物化回退路径。
        auto result = run(plan.children.front());
// 一次性执行子计划得到完整结果。
        const auto size = result.at("rows").size();
// 结果总行数。
        const auto begin = std::min<std::uint64_t>(plan.offset, size);
// 起始下标取 min(offset, size)，防止越界。
        const auto count = std::min<std::uint64_t>(plan.limit.value_or(size), size - begin);
// 可返回行数取 min(limit, size - begin)。
        json rows = json::array();
// 构造物化 Limit 的结果行数组。
        for (std::uint64_t i = 0; i < count; ++i) rows.push_back(std::move(result["rows"][begin + i]));
// 从 begin 开始复制 count 行。
        result["rows"] = std::move(rows);
// 用截断后的行替换原结果。
        result["kind"] = "Limit";
// 标记结果为 Limit 节点。
        result["resourceUsage"] = {{"kind", "Limit"}, {"rows", rows.size()}};
// 记录资源使用情况。
        return result;
// 返回物化 Limit 结果。
    }
// Limit 分支结束。
    if (plan.kind == "Distinct") {
// Distinct 分支：对投影结果做去重。
        try {
            auto stream = openRowStream(plan);
            json rows = json::array(), row;
            while (stream->next(row)) rows.push_back(std::move(row));
            stream->close();
            auto usage = stream->resourceUsage();
            json columns = json::array();
            for (const auto& column : plan.output) columns.push_back(column.name);
            return {{"kind", "Distinct"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
                {"affectedRows", 0}, {"resourceUsage", std::move(usage)}};
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::InvalidArgument) throw;
        }
        if (plan.children.size() != 1) fail("Distinct requires one child");
        auto result = run(plan.children.front());
// 执行投影子节点。
        std::set<json> seen;
// 用有序集合记录已经出现过的行，重复行不会插入。
        json unique = json::array();
// 保存去重后的结果行。
        for (auto& row : result.at("rows")) if (seen.insert(row).second) unique.push_back(std::move(row));
// 逐行尝试插入 seen；插入成功说明第一次出现，才加入结果。
        result["rows"] = std::move(unique);
// 用去重结果替换原行数组。
        result["kind"] = "Distinct";
// 标记结果为 Distinct 节点。
        result["resourceUsage"] = {{"kind", "Distinct"}, {"rows", result.at("rows").size()}, {"external", false}};
        return result;
// 返回去重结果。
    }
// Distinct 分支结束。
    json result = {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}};
// 构造各类节点的统一结果骨架：kind、列名、行集合和影响行数。
    if (plan.kind == "Aggregate") {
// Aggregate 节点执行分支。
        try {
            auto stream = openRowStream(plan);
            json row;
            while (stream->next(row)) result["rows"].push_back(std::move(row));
            stream->close();
            for (const auto& column : plan.output) result["columns"].push_back(column.name);
            result["resourceUsage"] = stream->resourceUsage();
            return result;
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::InvalidArgument) throw;
        }
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 按计划输出模式填写列名。
        result["rows"] = aggregateRows(plan);
// 调用 aggregateRows 完成分组聚合。
        result["resourceUsage"] = {{"kind", "Aggregate"}, {"rows", result.at("rows").size()},
// 记录聚合资源使用情况。
            {"groups", result.at("rows").size()}, {"external", false}};
// 行数同时就是分组数；此处未使用外排，external 固定为 false。
        return result;
// 返回聚合结果。
    }
// Aggregate 分支结束。
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
// Filter/SemiJoin/AntiJoin/Apply 在普通执行路径下统一按过滤处理。
        if (plan.children.size() != 1) fail("Filter requires one child");
// 过滤类节点必须恰好有一个输入。
        auto input = run(plan.children.front());
// 先完整执行输入子节点。
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 复制输出列名。
        for (auto& row : input["rows"]) if (accepted(evaluate(plan.predicate, row))) result["rows"].push_back(std::move(row));
// 逐行求谓词，接受的行移入结果。
        result["resourceUsage"] = {{"kind", "Filter"}, {"rows", result.at("rows").size()}};
// 记录过滤节点的资源使用。
        return result;
// 返回过滤结果。
    }
// 过滤类分支结束。
    if (plan.kind == "Project" && plan.children.size() == 1 &&
// 特判“Project 套 Aggregate”或“Project 套 Filter 套 Aggregate”的常见聚合投影路径。
        (plan.children.front().kind == "Aggregate" || (plan.children.front().kind == "Filter" &&
// 孩子是 Aggregate，或是只包了一层 Filter 的 Aggregate。
         plan.children.front().children.size() == 1 && plan.children.front().children.front().kind == "Aggregate"))) {
// 这个条件用于确认聚合位于 Project 的直接输入链上。
        const auto input = run(plan.children.front());
// 执行内层聚合及可能的过滤。
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 复制外层投影的列名。
        for (const auto& row : input.at("rows")) {
// 对聚合后的每一行做投影。
            json projected = json::array();
// 保存投影结果行。
            for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
// 依次求值外层投影表达式。
            result["rows"].push_back(std::move(projected));
// 把投影后的行加入结果。
        }
// 聚合行遍历结束。
        json projectUsage = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
// 构造 Project 的资源使用信息。
        if (input.contains("resourceUsage")) projectUsage["child"] = input.at("resourceUsage");
// 如果子节点提供了资源信息，把它嵌到 Project 下面形成调用链。
        result["resourceUsage"] = std::move(projectUsage);
// 写回资源使用字段。
        return result;
// 返回投影结果。
    }
// 聚合投影快速路径结束。
    if (plan.kind == "Project" && plan.children.size() == 1 && plan.children.front().kind == "Limit" &&
// 特判 Project 套 Limit 0：内层恒空，外层只需返回列名。
        plan.children.front().limit && *plan.children.front().limit == 0) {
// 确认孩子是 Limit 且上限为 0。
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 填写输出列名。
        return result;
// 直接返回零行结果。
    }
// Limit 0 快速路径结束。
    // X09 3.3: 外层 Select 投影于一个“成形的”子计划（派生表）之上 —— 先物化内层
// X09 3.3：外层 Select 投影在派生表等“成形子计划”之上时，需要先物化内层关系。
    // 关系，再对外层投影求值。普通 Select（Filter 下接裸 Scan）不受影响。
// 普通 Select（Filter 下接裸 Scan）不走这条路径，保持原有流式扫描。
    // X09 3.3: 外层 Select 投影于一个“成形的”子计划（派生表）之上 —— 先物化内层
    // 关系，再对外层投影求值。普通 Select（Filter 下接裸 Scan）不受影响。
    if (plan.kind == "Project" && plan.children.size() == 1) {
// 只有一个孩子的 Project 才需要判断是否属于成形子计划。
        std::function<bool(const sql::LogicalPlan&)> subplanRoot;
// 定义递归判断函数，用来识别子计划根是否为成形关系。
        subplanRoot = [&subplanRoot](const sql::LogicalPlan& node) -> bool {
// 递归 lambda 绑定自身引用，便于在函数体内继续调用。
            if (node.kind == "Project" || node.kind == "Aggregate" || node.kind == "Distinct" ||
// Project、Aggregate、Distinct、Sort、Limit 都会产生一个完整关系，视为成形子计划。
                node.kind == "Sort" || node.kind == "Limit") return true;
// 以上五种节点一旦出现，外层投影必须先物化内层。
            if (node.kind == "Filter") return !node.children.empty() && subplanRoot(node.children.front());
// Filter 只有在其孩子也是成形子计划时才算成形。
            return false;
// 其他节点不算成形子计划。
        };
// 递归 lambda 定义结束。
        // 孩子是连接时，先物化连接结果，再对外层投影表达式逐行求值。
        // 必须单独处理：派生表参与连接时连接节点的 table 字段是派生表别名
        // （FROM (SELECT ...) x JOIN t y 里的 x），目录里没有这张表，若落到下面那条
        // 按 plan.table 找物理表的通用路径，会报 Plan references missing table。
        // joinRows 支持连接及其输入子树（含派生表 Project），因此这里直接复用。
        if (plan.children.front().kind == "NestedLoopJoin" || plan.children.front().kind == "HashJoin" ||
            plan.children.front().kind == "LeftJoin" || plan.children.front().kind == "RightJoin" ||
            plan.children.front().kind == "FullJoin") {
            for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 填写外层输出列名。
            for (const auto& joinedRow : joinRows(plan.children.front())) {
// 物化连接并逐行投影。
                const auto asJson = rowJson(joinedRow);
// 转成 JSON 便于表达式求值。
                json projected = json::array();
// 投影结果行。
                if (!plan.projections.empty()) for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, asJson));
// 有显式投影时逐表达式求值。
                else for (const auto& column : plan.output) {
// 否则按输出列号取列。
                    if (column.columnId >= asJson.size()) fail("Join projection outside row");
// 越界说明计划与输入模式不一致。
                    projected.push_back(asJson.at(column.columnId));
// 取出对应单元格。
                }
// 投影分支结束。
                result["rows"].push_back(std::move(projected));
// 收集结果行。
            }
// 连接结果遍历结束。
            result["kind"] = "Project";
// 标记结果类型。
            result["resourceUsage"] = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
// 附带资源使用信息。
            return result;
// 返回连接加投影的结果。
        }
// 连接孩子特判结束。
        if (subplanRoot(plan.children.front())) {
// 孩子确实是成形子计划时走物化投影路径。
            for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 填写外层输出列名。
            const auto sub = run(plan.children.front());
// 先执行内层子计划得到完整关系。
            for (const auto& row : sub.at("rows")) {
// 逐行对外层投影表达式求值。
                json projected = json::array();
// 准备投影后的行。
                for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
// 求值每个投影表达式。
                result["rows"].push_back(std::move(projected));
// 收集投影结果行。
            }
// 内层行遍历结束。
            json projectUsage = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
// 构造 Project 资源使用信息。
            if (sub.contains("resourceUsage")) projectUsage["child"] = sub.at("resourceUsage");
// 把内层资源使用信息挂到 child 字段。
            result["resourceUsage"] = std::move(projectUsage);
// 写回资源使用字段。
            return result;
// 返回物化投影结果。
        }
// 成形子计划分支结束。
    }
// Project 物化特判结束。
    if (plan.kind == "CreateIndex") {
// CreateIndex 节点：建索引前先校验唯一性，再写目录并重建运行期索引。
        const catalog::StoredTable* stored = nullptr;
// 先定位目标表。
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) stored = &candidate;
// 按表名做大小写不敏感查找。
        if (!stored) fail("Index table does not exist");
// 目标表不存在，无法建索引。
        const auto* definition = catalog_.view().find(stored->definition.table);
// 取出目录视图中的表定义。
        if (!definition) fail("Index table definition missing");
// 目录视图缺少定义，说明目录状态不一致。
        std::vector<std::size_t> columns;
// 保存索引列在表中的下标。
        for (const auto& name : plan.indexColumns) columns.push_back(catalog::resolveColumnIndex(*definition, name));
// 把每个索引列名解析成列下标。
        // 唯一索引建造三阶段：先在候选树里 build，再 validate（结构/条目数/唯一性），
        // 只有全部通过才在 publish 阶段登记目录与运行实例；任一步失败随写批次回滚。
        auto candidate = std::make_unique<RuntimeIndex>(plan.indexName, stored->definition.table, columns,
                                                        plan.uniqueIndex, pageFileIndexes_);
        std::size_t entries = 0;
        const auto problems = buildIndexEntries(*candidate, stored->id, rowSchema(stored->definition), &entries);
        if (!problems.empty()) {
            const bool duplicate = std::any_of(problems.begin(), problems.end(), [](const std::string& problem) {
                return problem.rfind("duplicate keys", 0) == 0;
            });
// 唯一性预检查扫描结束。
            if (plan.uniqueIndex && duplicate) fail("UNIQUE index contains duplicate keys");
            const auto detail = "Index build validation failed: " + plan.indexName + " (" + joinProblems(problems) + ")";
            fail(detail.c_str());
        }
// 唯一性检查分支结束。
        sql::Statement indexDefinition;
// 构造目录层需要的 CreateIndex 语句描述。
        indexDefinition.kind = "CreateIndex";indexDefinition.indexName = plan.indexName;
// 填写语句类型和索引名。
        indexDefinition.uniqueIndex = plan.uniqueIndex;indexDefinition.table = plan.table;
// 填写唯一性标志和目标表名。
        indexDefinition.indexColumns = plan.indexColumns;
// 填写索引列列表。
        catalog_.createIndex(indexDefinition);
// 写入目录，生成持久化索引定义。
        indexes_.push_back(std::move(candidate));
        // 内存引擎需把发布后的树写回快照，否则后续失败回滚无法还原运行时。
        if (!pageFileIndexes_) persistMemoryIndexes(stored->id);
        result["kind"] = "CreateIndex";
// 结果类型标记为 CreateIndex。
        result["entriesBuilt"] = entries;
        return result;
    }
// CreateIndex 分支结束。
    if (plan.kind == "DropIndex") {
// DropIndex 节点：先删目录定义，再清理运行期索引与索引页。
        sql::Statement definition;
// 构造目录层的 DropIndex 语句描述。
        definition.kind = "DropIndex";definition.indexName = plan.indexName;definition.table = plan.table;
// 填写类型、索引名和可选的表名。
        catalog_.dropIndex(definition);
// 从目录中移除索引定义。
        for (const auto& index : indexes_)
// 遍历当前已打开的运行期索引。
            if (key(index->name) == key(plan.indexName) && (plan.table.empty() || key(index->table) == key(plan.table)))
// 索引名匹配，且表名未限定或也匹配时命中。
                clearIndexPages(indexOwnerId(index->table, index->name));
// 清理该索引占用的页，并把页标记为空闲。
        indexes_.erase(std::remove_if(indexes_.begin(), indexes_.end(), [&](const auto& index) {
// 从运行期索引列表中删除所有匹配项。
            return key(index->name) == key(plan.indexName) && (plan.table.empty() || key(index->table) == key(plan.table));
// remove_if 的匹配条件与前面一致。
        }), indexes_.end());
// erase 真正删除被移出的元素。
        result["kind"] = "DropIndex";
// 结果类型标记为 DropIndex。
        return result;
    }
// DropIndex 分支结束。
    if (plan.kind == "CreateTable") {
// CreateTable 节点：把逻辑计划中的表结构转换成目录语句并落库。
        sql::Statement definition;
// 构造目录层的 CreateTable 语句。
        definition.kind = "CreateTable";
// 标记语句类型。
        definition.table = plan.table;
// 表名。
        definition.keys = plan.keys;
// 主键定义。
        definition.foreignKeys = plan.foreignKeys;
// 外键定义。
        definition.constraintNames = plan.constraintNames;
// 约束名定义。
        for (const auto& check : plan.checkDefinitions) definition.checks.push_back(sql::deserializeExpression(check));
// 反序列化 CHECK 表达式并放入目录语句。
        for (const auto& column : plan.output) definition.columns.push_back({column.name, column.type, column.nullable, column.defaultValue, column.primaryKey, column.unique, column.references});
// 把计划输出模式中的列定义转换为目录列定义。
        catalog_.create(definition);
// 在目录中创建表及其系统记录。
        return result;
    }
// CreateTable 分支结束。
    const catalog::StoredTable* table = nullptr;
// 后续读写语句都需要先定位目标表。
    for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
// 按表名查找目录中的已存表。
    if (!table) fail("Plan references missing table");
// 找不到表说明计划与目录不一致。
    const auto schema = rowSchema(table->definition);
// 生成堆表行模式，用于读写行。
    if (plan.kind == "IndexScan") {
// IndexScan 节点：按索引键或范围取出候选行引用。
        const RuntimeIndex* index = nullptr;
// 找到计划指定的运行期索引。
        for (const auto& candidate : indexes_) if (key(candidate->table) == key(plan.table) && key(candidate->name) == key(plan.indexName)) index = candidate.get();
// 表名和索引名都匹配才命中。
        if (!index) fail("IndexScan references missing runtime index");
// 运行期索引缺失说明索引未打开或计划过期。
        std::vector<storage::RowRef> refs;
// 保存索引命中的行引用。
        if (!plan.indexRangeOperator.empty()) {
// 有范围条件时走 range 查询。
            storage::IndexKey key;
// 构造等值前缀键：多列索引先匹配前面的等值列。
            for (const auto& value : plan.indexValues)
// 逐个把等值列值转换成索引键分量。
                key.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
// 追加等值键分量。
            key.values.push_back(indexValue(plan.indexRangeValue.at("value"), plan.indexRangeValue.at("type").get<std::string>()));
// 追加范围端点作为最后一个键分量。
            const auto& op = plan.indexRangeOperator;
// 取出范围运算符。
            if (op == ">") refs = index->range(key, false, std::nullopt, true);
// “>”查询：下界排他，无上界。
            else if (op == ">=") refs = index->range(key, true, std::nullopt, true);
// “>=”查询：下界包含，无上界。
            else if (op == "<") refs = index->range(std::nullopt, true, key, false);
// “<”查询：无下界，上界排他。
            else refs = index->range(std::nullopt, true, key, true);
// “<=”查询：无下界，上界包含。
        } else if (!plan.indexValues.empty()) {
// 没有范围条件但有等值列时走精确查找。
            storage::IndexKey key;
// 构造完整等值键。
            for (const auto& value : plan.indexValues)
// 把每个等值列值转换成索引键分量。
                key.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
// 追加到键中。
            refs = index->search(key);
// 在索引中做精确查找。
        } else {
// 既没有范围也没有显式键时，从谓词中提取比较条件。
        const auto& predicate = plan.predicate;
// 取出索引扫描谓词。
        if (!predicate.is_object()) fail("IndexScan requires a predicate");
// 谓词必须是对象结构。
        const auto op = predicate.value("operator", "");
// 取出比较运算符。
        if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=") fail("IndexScan requires an indexed comparison");
// 只支持可与索引区间对应的五种比较。
        const auto& right = predicate.at("right");
// 取比较的右操作数。
        if (!right.is_object() || right.value("kind", "") != "Literal") fail("IndexScan requires a literal key");
// 索引扫描要求右操作数是字面量，保证查询前就能确定键值。
        const auto key = storage::IndexKey{{indexValue(right.at("value"), right.at("type").get<std::string>())}};
// 把字面量转换成单列索引键。
        if (op == "=") refs = index->search(key);
// “=”走精确查找。
        else if (op == ">") refs = index->range(key, false, std::nullopt, true);
// “>”查询。
        else if (op == ">=") refs = index->range(key, true, std::nullopt, true);
// “>=”查询。
        else if (op == "<") refs = index->range(std::nullopt, true, key, false);
// “<”查询。
        else refs = index->range(std::nullopt, true, key, true);
// 剩下“<=”查询。
        }
// 谓词提取分支结束。
        for (const auto ref : refs) result["rows"].push_back(rowJson(heap_.read(table->id, schema, ref)));
// 把每个候选行引用回表读成完整行，并转成 JSON。
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 按输出模式填写列名。
        return result;
// 返回索引扫描结果。
    }
// RELEASE SAVEPOINT 分支结束。
// IndexScan 分支结束。

    const auto checkConstraints = plan.checks;
// 取出计划携带的 CHECK 约束表达式。
    const auto checkRows = [&](const storage::Row& row) {
// 定义单行 CHECK 校验函数。
        for (std::size_t i = 0; i < checkConstraints.size(); ++i) {
// 逐个检查约束。
            const auto& check = checkConstraints[i];
// 取出当前 CHECK 表达式。
            const auto value = evaluate(check, row);
// 对当前行求值。
            if (value.is_null() || (value.is_boolean() && value.get<bool>())) continue;
// SQL CHECK 语义：结果为 NULL 或 true 都算通过。
            throw MiniSqlError(ErrorCode::Execution, "CHECK constraint failed" + sql::constraintSuffix(table->definition.constraintNames, "check", i));
// 结果为 false 时抛出执行错误，并附带约束名。
        }
// 当前行约束检查结束。
    };
// checkRows lambda 结束。
    const auto references = sql::allForeignKeys(table->definition);
// 汇总表定义中的全部外键。
    const auto isSelfReference = [&](const sql::ForeignKey& reference) { return key(reference.table) == key(table->definition.table); };
// 判断某个外键是否引用本表自身。
    const bool hasSelfReferences = std::any_of(references.begin(), references.end(), isSelfReference);
// 预计算是否存在自引用外键，供后续批量校验使用。
    const auto checkSelfReferences = [&](const std::vector<storage::Row>& finalRows) {
// 定义自引用外键的整批校验函数。
        for (std::size_t r = 0; r < references.size(); ++r) {
// 遍历全部外键定义。
            const auto& reference = references[r];
// 取出当前外键。
            if (!isSelfReference(reference)) continue;
// 只处理自引用外键，其他外键由逐行函数负责。
            std::vector<std::size_t> parentIndices, childIndices;
// 保存父键列下标和子键列下标。
            const auto* definition = catalog_.view().find(table->definition.table);
// 取出本表定义，用于列名解析。
            for (const auto& name : reference.columns) childIndices.push_back(catalog::resolveColumnIndex(*definition, name));
// 子列名解析成行内下标。
            for (const auto& name : reference.referencedColumns) parentIndices.push_back(catalog::resolveColumnIndex(*definition, name));
// 被引用列名解析成行内下标。
            auto tuple = [&](const storage::Row& row, const std::vector<std::size_t>& indices) {
// 定义从一行中按给定下标抽取键元组的辅助函数。
                auto value = json::array();
// 键元组用 JSON 数组表示。
                for (const auto index : indices) value.push_back(cell(row[index]));
// 按下标构造元组。
                return value;
// 返回元组。
            };
// 辅助 lambda 结束。
            std::set<json> parentKeys;
// 收集最终状态中所有父键。
            for (const auto& row : finalRows) parentKeys.insert(tuple(row, parentIndices));
// 用最终行集合构建父键集合。
            for (const auto& row : finalRows) {
// 再遍历最终行做子键检查。
                const auto value = tuple(row, childIndices);
// 取出当前行的子键。
                if (std::any_of(value.begin(), value.end(), [](const json& item) { return item.is_null(); })) continue;
// 子键含 NULL 时外键约束不适用，跳过。
                if (!parentKeys.contains(value)) throw MiniSqlError(ErrorCode::Execution, "Self-referencing FOREIGN KEY constraint failed" + sql::foreignKeySuffix(table->definition, r));
// 子键在最终父键集合中不存在，抛出自引用外键错误。
            }
// 表名建议分支结束。
// 当前行检查结束。
        }
// 当前自引用外键检查结束。
    };
// checkSelfReferences lambda 结束。
    const auto checkForeignKeys = [&](const storage::Row& row) {
// 定义非自引用外键的逐行校验函数。
        for (std::size_t r = 0; r < references.size(); ++r) {
// 遍历全部外键定义。
            const auto& reference = references[r];
// 取出当前外键。
            if (isSelfReference(reference)) continue;
// 自引用外键由整批函数处理，这里跳过。
            auto values = json::array();bool hasNull = false;
// 保存子键值，并标记是否含 NULL。
            for (const auto& column : reference.columns) {
// 逐个子列取值。
                const auto index = catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), column);
// 解析子列下标。
                values.push_back(cell(row[index]));hasNull = hasNull || values.back().is_null();
// 记录子键分量与 NULL 标记。
            }
// 子键遍历结束。
            if (hasNull) continue;
// 子键含 NULL 时外键不检查。
            const catalog::StoredTable* parent = nullptr;
// 查找被引用的父表。
            for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(reference.table)) parent = &candidate;
// 按表名匹配父表。
            if (!parent) fail("Referenced table does not exist");
// 父表不存在说明外键定义已失效。
            const auto* parentDefinition = catalog_.view().find(parent->definition.table);
// 取父表目录定义。
            std::vector<std::size_t> parentIndices;
// 保存被引用列在父表中的下标。
            for (const auto& column : reference.referencedColumns) parentIndices.push_back(catalog::resolveColumnIndex(*parentDefinition, column));
// 把被引用列名解析成父表列下标。
            const auto parentSchema = rowSchema(parent->definition);
// 生成父表行模式。
            bool found = false;
// 标记是否找到匹配的父键。
            heap_.scan(parent->id, parentSchema, [&](storage::RowRef, const storage::Row& parentRow) {
// 扫描父表查找键值。
                if (found) return;
// 已经找到就可以提前结束扫描。
                auto tuple = json::array();
// 构造父表键元组。
                for (const auto index : parentIndices) tuple.push_back(cell(parentRow[index]));
// 按被引用列下标填元组。
                found = tuple == values;
// 元组相等即外键满足。
            });
// 父表扫描结束。
            if (!found) throw MiniSqlError(ErrorCode::Execution, "FOREIGN KEY constraint failed" + sql::foreignKeySuffix(table->definition, r));
// 没有匹配父键时抛出外键错误。
        }
// 当前外键检查结束。
    };
// checkForeignKeys lambda 结束。
    std::vector<std::vector<std::size_t>> uniqueColumns;
// 汇总所有需要唯一性检查的列组合：单列主键/唯一列，以及显式多列键。
    const auto restrictParent = [&](storage::RowRef oldRef, const storage::Row& oldRow, const storage::Row* replacement) {
// RESTRICT 语义：删除或修改父键前，检查是否仍被其他行引用。
        for (const auto& child : catalog_.tables()) {
// 遍历所有子表。
            const auto childReferences = sql::allForeignKeys(child.definition);
// 取出子表全部外键。
            for (std::size_t r = 0; r < childReferences.size(); ++r) {
// 遍历子表的每个外键。
                const auto& reference = childReferences[r];
// 取当前外键定义。
                if (key(reference.table) != key(table->definition.table)) continue;
// 只关心引用当前父表的外键。
                auto oldValues = json::array(), newValues = json::array();bool hasNull = false;
// 保存旧父键和新父键，并标记旧键是否含 NULL。
                for (const auto& column : reference.referencedColumns) {
// 逐列构造父键元组。
                    const auto index = catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), column);
// 解析被引用列在父表中的下标。
                    oldValues.push_back(cell(oldRow[index]));hasNull = hasNull || oldValues.back().is_null();
// 记录旧键分量与 NULL 标记。
                    if (replacement) newValues.push_back(cell((*replacement)[index]));
// 如果是 UPDATE，同时记录新键分量。
                }
// 父键列遍历结束。
                if (hasNull || (replacement && oldValues == newValues)) continue;
// 旧键含 NULL，或修改后父键没变，都不需要检查子表。
                std::vector<std::size_t> childIndices;
// 保存子表外键列下标。
                for (const auto& column : reference.columns)
// 逐个解析子表外键列。
                    childIndices.push_back(catalog::resolveColumnIndex(*catalog_.view().find(child.definition.table), column));
// 解析子列名到行内下标。
                heap_.scan(child.id, rowSchema(child.definition), [&](storage::RowRef childRef, const storage::Row& childRow) {
// 扫描子表寻找引用旧父键的行。
                    // 当前行的自引用由候选最终状态检查；其他行仍执行 RESTRICT。
// 当前行自身的自引用交给候选最终状态检查，这里跳过。
                    // 当前行的自引用由候选最终状态检查；其他行仍执行 RESTRICT。
                    if (child.id == table->id && childRef.page.id == oldRef.page.id && childRef.page.generation == oldRef.page.generation &&
// 先比较表身份和页身份。
                        childRef.slot.slot == oldRef.slot.slot && childRef.slot.generation == oldRef.slot.generation) return;
// 再比较槽位身份，完全一致才是当前正在修改的行。
                    auto tuple = json::array();
// 构造子表引用键元组。
                    for (const auto index : childIndices) tuple.push_back(cell(childRow[index]));
// 按子表外键列下标填元组。
                    if (tuple == oldValues) throw MiniSqlError(ErrorCode::Execution, "FOREIGN KEY RESTRICT: parent key is referenced" + sql::foreignKeySuffix(child.definition, r));
// 子表仍有行引用旧父键时违反 RESTRICT，报错。
                });
// 子表扫描结束。
            }
// 当前外键检查结束。
        }
// 子表遍历结束。
    };
// restrictParent lambda 结束。
    std::vector<bool> primaryKeys;
// 标记每个唯一约束是否来自主键（主键不允许 NULL）。
    std::vector<std::string> uniqueNames;
// 保存唯一约束的显示名称后缀。
    for (std::size_t i = 0; i < schema.size(); ++i)
// 先处理列级的 PRIMARY KEY 和 UNIQUE。
        if (table->definition.columns[i].primaryKey || table->definition.columns[i].unique) {
// 列被标记为主键或唯一列时加入检查列表。
            uniqueColumns.push_back({i});primaryKeys.push_back(table->definition.columns[i].primaryKey);
// 单列组合加入 uniqueColumns，并记录是否主键。
            uniqueNames.push_back(sql::constraintSuffix(table->definition.constraintNames, table->definition.columns[i].primaryKey ? "primaryKey" : "unique", i));
// 生成约束名后缀，便于错误信息定位。
        }
// 列级唯一约束收集结束。
    for (std::size_t i = 0; i < table->definition.keys.size(); ++i) {
// 再处理表级多列键定义。
        const auto& constraint = table->definition.keys[i];
// 取出当前表级键约束。
        std::vector<std::size_t> indices;
// 保存该约束涉及的列下标。
        for (const auto& name : constraint.columns)
// 逐个解析约束列名。
            indices.push_back(catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), name));
// 把列名解析成行内下标。
        uniqueColumns.push_back(std::move(indices));primaryKeys.push_back(constraint.primary);
// 多列组合加入唯一检查列表，并记录是否主键。
        uniqueNames.push_back(sql::constraintSuffix(table->definition.constraintNames, "key", i));
// 生成表级键约束名后缀。
    }
// 表级键约束收集结束。
    std::vector<std::set<json>> uniqueValues(uniqueColumns.size());
// 为每个唯一组合准备一个已出现值集合。
    const auto checkUnique = [&](const storage::Row& row) {
// 定义单行唯一性检查函数。
        for (std::size_t i = 0; i < uniqueColumns.size(); ++i) {
// 逐个唯一约束检查。
            auto value = json::array();bool hasNull = false;
// 构造当前约束的键值，并记录 NULL。
            for (const auto index : uniqueColumns[i]) {
// 逐列取键值。
                value.push_back(cell(row[index]));hasNull = hasNull || value.back().is_null();
// 添加键分量并更新 NULL 标记。
            }
// 键中含 NULL 时特殊处理。
            if (hasNull) {
// 主键分量不能为 NULL。
                if (primaryKeys[i]) throw MiniSqlError(ErrorCode::Execution, "PRIMARY KEY cannot be NULL" + uniqueNames[i]);
// 非主键的 UNIQUE 允许多个 NULL，直接跳过。
                continue;
// NULL 分支结束。
            }
// 记录该键；插入失败说明已经存在重复值。
            if (!uniqueValues[i].insert(value).second)
// 抛出唯一约束冲突错误。
                throw MiniSqlError(ErrorCode::Execution, "UNIQUE constraint failed on key " + std::to_string(i + 1) + uniqueNames[i]);
// 当前约束检查结束。
        }
// 所有唯一约束检查结束。
    };
// checkUnique lambda 结束。
    if (plan.kind == "Insert") {
// INSERT 分支：逐行构造强类型行，完成约束检查后统一写入堆表。
        std::vector<storage::Row> candidates;
// 保存所有待插入行；先全部校验，避免部分插入成功。
        if (!uniqueColumns.empty())
// 存在唯一约束时，先把已有数据装入唯一值集合。
            heap_.scan(table->id, schema, [&](storage::RowRef, const storage::Row& existing) { checkUnique(existing); });
// 扫描当前表，把现存行的键登记进唯一集合。
        const auto prepareRow = [&](const json& values, const json& expressions) {
// 定义把一行 JSON 值与表达式转换成存储行的函数。
            storage::Row row;
// 准备内部存储行。
            if (values.size() != schema.size()) fail("Insert plan schema mismatch");
// 值列数必须与表模式一致。
            if (!expressions.empty() && expressions.size() != schema.size()) fail("Insert expression schema mismatch");
// 表达式列数若存在也必须与模式一致。
            for (std::size_t i = 0; i < schema.size(); ++i) {
// 逐列转换。
                const auto value = expressions.empty() ? values[i] : evaluate(expressions[i], storage::Row{});
// 有表达式就现场求值，否则直接用传入值。
                if (value.is_null()) {
// 处理 NULL。
                    if (!table->definition.columns[i].nullable) throw MiniSqlError(ErrorCode::Execution, "NOT NULL constraint failed" + catalog::notNullConstraintSuffix(*catalog_.view().find(table->definition.table), i));
// 列不允许 NULL 时抛 NOT NULL 约束错误。
                    row.emplace_back(std::monostate{});
// 允许 NULL 时放入 monostate 作为空值。
                } else if (schema[i] == storage::ColumnType::Int) row.emplace_back(value.get<std::int32_t>());
// Int 列转成 32 位整数。
                else if (schema[i] == storage::ColumnType::Bigint) row.emplace_back(value.get<std::int64_t>());
// Bigint 列转成 64 位整数。
                else if (schema[i] == storage::ColumnType::Bool) row.emplace_back(value.get<bool>());
// Bool 列转成布尔值。
                else if (schema[i] == storage::ColumnType::Float) {
// Float 列需要做有限性与范围检查。
                    const auto& expression = expressions.empty() ? json::object() : expressions[i];
// 取表达式位置信息用于错误定位。
                    row.emplace_back(storedFloat(value, {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})}));
// 转换并保存浮点值。
                }
// Float 分支结束。
                else if (schema[i] == storage::ColumnType::Decimal) {
// Decimal 列需要按定点类型转换并检查精度。
                    const auto& expression = expressions.empty() ? json::object() : expressions[i];
// 取表达式位置信息。
                    row.emplace_back(storedDecimal(value, schema[i], {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})}));
// 转换并保存定点数。
                } else {
// 其余是字符串类列。
                    const auto text=value.get<std::string>();
// 取出字符串值。
                    if (schema[i] == storage::ColumnType::BoundedVarchar) {
// 有长度上限的 VARCHAR 需要先校验长度。
                        const auto& expression=expressions.empty() ? json::object() : expressions[i];
// 取表达式位置信息。
                        validateVarchar(text,schema[i].maxLength,{expression.value("line",std::size_t{0}),expression.value("column",std::size_t{0})});
// 校验字符串字节长度是否超过声明上限。
                    }
// 长度校验分支结束。
                    row.emplace_back(text);
// 保存字符串值。
                }
// 列类型分支结束。
            }
// 所有列转换结束。
            (void)storage::encodeRow(row, schema);
// 试编码一次，提前发现行格式无法编码的问题。
            checkUnique(row);
// 跑唯一约束检查，把这一行的键登记进集合。
            checkRows(row);
// 跑 CHECK 约束检查。
            checkForeignKeys(row);
// 跑非自引用外键检查。
            candidates.push_back(std::move(row));
// 全部通过后把候选行放入待插入列表。
        };
// prepareRow lambda 结束。
        if (plan.insertRows.empty()) prepareRow(plan.values, plan.insertExpressions);
// 单行 INSERT 走 values/insertExpressions。
        else for (const auto& row : plan.insertRows) prepareRow(row.at("values"), row.at("expressions"));
// 多行 INSERT 逐行转换。
        if (hasSelfReferences) {
// 存在自引用外键时需要把“插入后的最终表状态”一起检查。
            std::vector<storage::Row> finalRows;
// 保存插入后的完整行集合。
            heap_.scan(table->id, schema, [&](storage::RowRef, const storage::Row& existing) { finalRows.push_back(existing); });
// 先放入表中已有行。
            finalRows.insert(finalRows.end(), candidates.begin(), candidates.end());
// 再追加本次待插入行。
            checkSelfReferences(finalRows);
// 对最终状态做自引用外键校验。
        }
// 自引用检查分支结束。
        for (const auto& row : candidates) validateUniqueIndexes(table->id, row);
// 对每个候选行检查运行期唯一索引，避免破坏索引语义。
        for (const auto& row : candidates) {
            const auto ref = heap_.insert(table->id, schema, row);
            insertIndexEntries(table->id, row, ref);
        }
        persistMemoryIndexes(table->id);
        if (!candidates.empty()) ++indexVersion_;
        heap_.flush();
// 刷堆表数据，保证插入在返回前可见。
        result["affectedRows"] = candidates.size();
// 返回实际插入行数。
        return result;
    }
// INSERT 分支结束。
    if (plan.kind != "Project" && plan.kind != "Delete" && plan.kind != "Update") fail("Unsupported root plan");
// 只有 Project、Delete、Update 能作为根计划继续后面的读写路径。
    if (plan.children.size() != 1) fail("Root plan requires one child");
// 根计划必须恰好有一个输入。
    if (plan.kind == "Project") {
// Project 根节点优先走流式路径。
        try {
// 尝试创建行流；若下层不支持再回退到物化路径。
            auto stream = openRowStream(plan);
// 为整棵 Project 计划创建行流。
            json rows = json::array();
// 保存流式读取到的行。
            json row;
// 复用同一个 JSON 对象接收每一行。
            while (stream->next(row)) rows.push_back(std::move(row));
// 一直读取直到流结束。
            stream->close();
// 关闭行流释放资源。
            json columns = json::array();
// 构造输出列名。
            for (const auto& column : plan.output) columns.push_back(column.name);
// 按计划输出模式填写列名。
            return {{"kind", "Project"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
// 返回流式 Project 结果。
                {"affectedRows", 0}, {"resourceUsage", stream->resourceUsage()}};
// 带上行流提供的资源使用信息。
        } catch (const MiniSqlError& error) {
// 捕获下层不支持行流的错误。
            if (error.code() != ErrorCode::InvalidArgument) throw;
// 只有 InvalidArgument 才回退；其他错误继续抛出。
        }
// try 块结束，继续走物化回退路径。
    }
// Project 流式尝试结束。
    const auto* input = &plan.children.front();
// 取得根计划的输入节点。
    std::vector<const json*> predicates;
    while (input->kind == "Filter") {
        predicates.push_back(&input->predicate);
        if (input->children.size() != 1) fail("Filter requires one child");
// 过滤节点必须恰好有一个孩子。
        input = &input->children.front();
// 继续向内找到真正的扫描或连接输入。
    }
// 过滤剥离结束。
    if (input->kind == "SemiJoin" || input->kind == "AntiJoin" || input->kind == "Apply") {
        predicates.push_back(&input->predicate);
        if (input->children.size() != 1) fail("Filter requires one child");
        input = &input->children.front();
    }
    const auto acceptsPredicates = [&](const auto& row) {
        for (const auto* predicate : predicates) if (!accepted(evaluate(*predicate, row))) return false;
        return true;
    };
    const bool joined = (input->kind == "NestedLoopJoin" || input->kind == "HashJoin" || input->kind == "LeftJoin" || input->kind == "RightJoin" || input->kind == "FullJoin") && plan.kind == "Project";
// 判断输入是否为可供 Project 直接消费的 JOIN 结果。
    if (!joined && ((input->kind != "SeqScan" && input->kind != "IndexScan") || key(input->table) != key(plan.table))) fail("Unsupported scan plan");
// 非 JOIN 情况下只接受与目标表一致的顺序扫描或索引扫描。
    for (const auto& column : plan.output) result["columns"].push_back(column.name);
// 按输出模式填写结果列名。
    if (input->kind == "IndexScan" && plan.kind == "Project") {
// 特判 IndexScan + Project 路径：先走索引扫描，再投影。
        const auto indexed = run(*input);
// 执行索引扫描得到候选行。
        for (const auto& row : indexed.at("rows")) {
// 逐行处理索引扫描结果。
            if (!acceptsPredicates(row)) continue;
            json projected = json::array();
// 准备投影结果行。
            if (!plan.projections.empty()) for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
// 有显式投影表达式时逐项求值。
            else for (const auto& column : plan.output) projected.push_back(row.at(column.columnId));
// 没有表达式时按输出列号直接取列。
            result["rows"].push_back(std::move(projected));
// 收集投影后的行。
        }
// 索引扫描行遍历结束。
        return result;
// 返回索引扫描投影结果。
    }
// IndexScan+Project 特判结束。
    std::vector<std::pair<storage::RowRef, storage::Row>> deletion;
    struct PendingUpdate {
        storage::RowRef ref;
        storage::Row original;
        storage::Row replacement;
    };
    std::vector<PendingUpdate> updates;
    const bool inspectSelfReferences = hasSelfReferences && (plan.kind == "Update" || plan.kind == "Delete");
// 只有更新/删除且存在自引用外键时，才需要检查最终表状态。
    std::vector<storage::Row> finalRows;
// 收集最终状态下的全部行，供自引用外键校验。
    const auto consume = [&](storage::RowRef ref, const storage::Row& row) {
// 定义扫描单行时的处理逻辑：判断是读、删还是改。
        checkCancelled();
// 每处理一行检查取消。
        if (!acceptsPredicates(row)) {
            if (plan.kind == "Update") checkUnique(row);
// UPDATE 场景仍需把保留行纳入唯一性集合，保证后续候选行检查正确。
            if (inspectSelfReferences) finalRows.push_back(row);
// 需要自引用检查时，保留行也要进入最终行集合。
            return;
// 保留行直接返回。
        }
// 保留分支结束。
        if (plan.kind == "Delete") { restrictParent(ref, row, nullptr); deletion.emplace_back(ref, row); return; }
        if (plan.kind == "Update") {
// UPDATE 场景：先计算所有赋值后的新行，完成全部校验后再真正替换。
            if (plan.columnMapping.size() != plan.projections.size()) fail("UPDATE assignment mapping mismatch");
// 赋值目标列数必须与赋值表达式数一致。
            auto replacement = row;
// 复制原行，逐列覆盖被赋值的列。
            for (std::size_t i = 0; i < plan.columnMapping.size(); ++i) {
// 遍历每个赋值目标。
                const auto index = plan.columnMapping[i];
// 取出目标列下标。
                if (index >= row.size()) fail("UPDATE column outside row");
// 目标列越界说明计划错误。
                const auto value = evaluate(plan.projections[i], row);
// 用原行求值赋值表达式；SQL UPDATE 的右侧读取旧值。
                if (value.is_null()) {
// 处理赋值为 NULL 的情况。
                    if (!table->definition.columns[index].nullable) throw MiniSqlError(ErrorCode::Execution, "NOT NULL constraint failed" + catalog::notNullConstraintSuffix(*catalog_.view().find(table->definition.table), index));
// 目标列不允许 NULL 时抛 NOT NULL 错误。
                    replacement[index] = std::monostate{};
// 允许 NULL 时写入 monostate。
                } else if (schema[index] == storage::ColumnType::Int) replacement[index] = value.get<std::int32_t>();
// Int 列转换。
                else if (schema[index] == storage::ColumnType::Bigint) replacement[index] = value.get<std::int64_t>();
// Bigint 列转换。
                else if (schema[index] == storage::ColumnType::Bool) replacement[index] = value.get<bool>();
// Bool 列转换。
                else if (schema[index] == storage::ColumnType::Float) {
// Float 列转换。
                    const auto& expression = plan.projections[i];
// 取赋值表达式位置。
                    replacement[index] = storedFloat(value, {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
// 转换并检查浮点有限性。
                }
// Float 分支结束。
                else if (schema[index] == storage::ColumnType::Decimal) {
// Decimal 列转换。
                    const auto& expression = plan.projections[i];
// 取赋值表达式位置。
                    replacement[index] = storedDecimal(value, schema[index], {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
// 转换并检查定点精度。
                } else {
// 字符串类列。
                    const auto text=value.get<std::string>();
// 取出字符串值。
                    if (schema[index] == storage::ColumnType::BoundedVarchar) {
// 有长度限制的 VARCHAR 需要校验。
                        const auto& expression=plan.projections[i];
// 取表达式位置信息。
                        validateVarchar(text,schema[index].maxLength,{expression.value("line",std::size_t{0}),expression.value("column",std::size_t{0})});
// 校验长度。
                    }
// 长度校验结束。
                    replacement[index]=text;
// 写入新字符串值。
                }
// 列类型分支结束。
            }
// 所有赋值列处理结束。
            (void)storage::encodeRow(replacement, schema);
// 试编码新行，提前发现格式错误。
            checkUnique(replacement);
// 检查更新后行的唯一约束。
            checkRows(replacement);
// 检查 CHECK 约束。
            checkForeignKeys(replacement);
// 检查普通外键。
            restrictParent(ref, row, &replacement);
// 检查更新父键时是否被其他行 RESTRICT 引用。
            if (inspectSelfReferences) finalRows.push_back(replacement);
// 需要自引用检查时，把新行加入最终行集合。
            updates.push_back({ref, row, std::move(replacement)});
            return;
// 当前更新行处理结束。
        }
// UPDATE 分支结束。
        json projected = json::array();
// 非 UPDATE/DELETE 的 Project 场景：做投影输出。
        if (!plan.projections.empty()) {
// 有显式投影表达式时逐项求值。
            for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
// 对当前行求值所有投影表达式。
        } else for (const auto& column : plan.output) {
// 没有显式表达式时按输出列号取列。
            if (column.columnId >= row.size()) fail("Projection outside row");
// 输出列号越界说明计划与行模式不一致。
            projected.push_back(cell(row[column.columnId]));
// 按列号取单元格并转换。
        }
// 投影分支结束。
        result["rows"].push_back(std::move(projected));
// 把投影行加入结果。
    };
// consume lambda 结束。
    if (joined) {
// 输入是 JOIN 时先物化连接结果再消费。
        for (const auto& row : joinRows(*input)) consume({}, row);
// 连接行没有单一行引用，传入空 RowRef。
    } else heap_.scan(table->id, schema, consume);
// 普通扫描直接把每个 RowRef 与行交给 consume。
    if (inspectSelfReferences) checkSelfReferences(finalRows);
// 更新/删除且存在自引用外键时，校验最终行集合。
    // 扫描及全部表达式检查完成后再写入，避免除零或行长错误造成前半批修改。
    for (const auto& update : updates) validateUniqueIndexes(table->id, update.replacement, update.ref);
    for (const auto& [ref, row] : deletion) {
        eraseIndexEntries(table->id, row, ref);
        heap_.erase(table->id, ref);
    }
    for (const auto& update : updates) {
        eraseIndexEntries(table->id, update.original, update.ref);
        const auto replacementRef = heap_.replace(table->id, schema, update.ref, update.replacement);
        insertIndexEntries(table->id, update.replacement, replacementRef);
    }
    if (!deletion.empty() || !updates.empty()) {
        persistMemoryIndexes(table->id);
        ++indexVersion_;
    }
    if (plan.kind == "Update") { heap_.flush(); result["affectedRows"] = updates.size(); }
// UPDATE 刷盘并报告影响行数。
    if (plan.kind == "Delete") { heap_.flush(); result["affectedRows"] = deletion.size(); }
// DELETE 刷盘并报告影响行数。
    if (plan.kind == "Project") result["resourceUsage"] = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
// Project 记录资源使用情况。
    return result;
// 返回最终结果。
}
const char* Database::transactionState() const {
// 返回事务状态字符串，供诊断接口读取。
    if (unavailable_) return "UNKNOWN";
// 写批次状态未知时统一报 UNKNOWN，提示调用方重开恢复。
    return transaction_ == TransactionState::Active ? "ACTIVE" : transaction_ == TransactionState::Aborted ? "ABORTED" : "IDLE";
// 活跃事务报 ACTIVE，已中止报 ABORTED，其余报 IDLE。
}
void Database::rollbackBatch() {
// 回滚整个写批次：撤销缓冲池临时页，并恢复目录与索引。
    try {
// 回滚过程中任何异常都说明提交状态不可确认。
        if (file_->writeBatchActive()) buffer_.rollbackWriteBatch();
// 有活动写批次时让缓冲池执行回滚。
        catalog_.reload();
// 重新加载目录，丢弃事务期间未提交的目录改动。
        savepoints_.clear();
// 清空事务保存点。
        reloadIndexRuntimes();
    } catch (const std::exception& error) {
        unavailable_ = true;
// 标记数据库实例不可用，后续操作必须重启恢复。
        throw MiniSqlError(ErrorCode::Storage, std::string("Commit state unknown; reopen for recovery: ") + error.what());
    }
}
nlohmann::json Database::run(const sql::LogicalPlan& plan) {
// 执行一个逻辑计划并记录节点级统计。
    checkCancelled();
// 执行前检查取消。
    const auto bufferBefore = buffer_.stats();
    const auto fileBefore = file_->ioStats();
    const auto started = std::chrono::steady_clock::now();
// 记录开始时间，用于计算耗时。
    auto result = runNode(plan);
// 真正执行计划节点。
    if (nodeStats_) {
// 只有开启节点统计时才记录。
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
// 计算本节点执行耗时（毫秒）。
        const auto bufferAfter = buffer_.stats();
        const auto fileAfter = file_->ioStats();
        const json io{{"available", true}, {"scope", "inclusive-subtree"},
            {"hits", bufferAfter.hits - bufferBefore.hits}, {"misses", bufferAfter.misses - bufferBefore.misses},
            {"pageReads", bufferAfter.pageReads - bufferBefore.pageReads},
            {"pageWrites", bufferAfter.pageWrites - bufferBefore.pageWrites},
            {"stagedPageReads", bufferAfter.stagedPageReads - bufferBefore.stagedPageReads},
            {"stagedPageWrites", bufferAfter.stagedPageWrites - bufferBefore.stagedPageWrites},
            {"diskReads", fileAfter.reads - fileBefore.reads}, {"diskWrites", fileAfter.writes - fileBefore.writes},
            {"ioErrors", fileAfter.errors - fileBefore.errors}};
        nodeStats_->push_back({{"kind", plan.kind}, {"table", plan.table},
            {"actualRows", result.value("rows", json::array()).size()}, {"durationMs", elapsed}, {"loops", 1},
            {"io", std::move(io)}});
    }
// 统计记录结束。
    return result;
}

nlohmann::json Database::runStatement(const sql::LogicalPlan& plan) {
// 执行一条完整语句，并在这里统一处理事务控制与写批次。
    correlatedRowsCache_.clear();
// 每次语句执行前清空相关子查询缓存，避免跨语句复用旧结果。
    if (plan.kind == "IndexInspect") return indexInspect(plan.table, plan.indexName);
// IndexInspect 是只读诊断语句，直接返回索引状态。
    if (plan.kind == "Checkpoint") {
// Checkpoint 语句要求事务空闲。
        if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "CHECKPOINT requires an idle transaction");
// 事务未空闲时不能做检查点。
        // 模糊检查点（MINISQL_FUZZY_CHECKPOINT=1）只记录检查点边界并保留日志，不强制刷出缓存。
        const bool fuzzy = std::getenv("MINISQL_FUZZY_CHECKPOINT") != nullptr;
        const bool archive = std::getenv("MINISQL_ARCHIVE_WAL") != nullptr;
        if (!fuzzy) buffer_.flushAll();
        storage::CheckpointOptions options;
        options.catalogVersion = catalogVersion_;
        options.indexVersion = indexVersion_;
        options.fuzzy = fuzzy;
        options.archive = archive;
        file_->checkpoint(options);
        ++checkpointCount_;
// 累计检查点次数。
        pendingAutoCheckpointWrites_ = 0;
// 清零待自动检查点的写入语句计数。
        pendingAutoCheckpointWalBytes_ = 0;
// 清零待自动检查点的 WAL 字节计数。
        lastCheckpointAt_ = std::chrono::steady_clock::now();
// 记录本次检查点时间（单调时钟）。
        lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
// 记录本次检查点的墙上时间戳。
            std::chrono::system_clock::now().time_since_epoch()).count());
// 把系统时钟毫秒数转成 uint64。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0},
                {"commitState", "committed"}, {"wal", fuzzy ? "retained" : "truncated"},
                {"fuzzy", fuzzy}, {"archived", archive}};
    }
// Checkpoint 分支结束。
    if (plan.kind == "Rollback") {
// Rollback 语句：必须有活动事务。
        if (transaction_ == TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "No active transaction");
// 空闲状态下没有事务可回滚。
        rollbackBatch();transaction_ = TransactionState::Idle;
// 执行写批次回滚，并把事务状态切回 Idle。
        transactionWriteStatements_ = 0;
// 清零事务内写语句计数。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "rolledBack"}};
// 返回回滚成功结果。
    }
// Rollback 分支结束。
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
// 事务已中止时只允许 ROLLBACK，其他语句一律拒绝。
    if (plan.kind == "Savepoint") {
// SAVEPOINT 语句要求事务活跃。
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "SAVEPOINT requires an active transaction");
// 非活跃事务不能创建保存点。
        if (plan.savepointName.empty()) throw MiniSqlError(ErrorCode::Transaction, "Savepoint name is required");
// 保存点名字不能为空。
        savepoints_[key(plan.savepointName)] = SavepointState{file_->savepoint(), catalog_.snapshot()};
// 同时保存页文件快照与目录快照，支持回滚到该点。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
// 返回保存点创建成功结果。
    }
// SAVEPOINT 分支结束。
    if (plan.kind == "ReleaseSavepoint") {
// RELEASE SAVEPOINT 语句。
        if (transaction_ != TransactionState::Active || !savepoints_.erase(key(plan.savepointName)))
// 事务必须活跃，且保存点必须存在。
            throw MiniSqlError(ErrorCode::Transaction, "Savepoint does not exist: " + plan.savepointName);
// 保存点不存在时抛事务错误。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
// 返回释放保存点成功结果。
// RELEASE SAVEPOINT 分支结束。
    }
// RELEASE SAVEPOINT 分支结束。
    if (plan.kind == "RollbackTo") {
// ROLLBACK TO SAVEPOINT 语句。
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "ROLLBACK TO requires an active transaction");
// 只有活跃事务才能回滚到保存点。
        const auto found = savepoints_.find(key(plan.savepointName));
// 查找指定保存点。
        if (found == savepoints_.end()) throw MiniSqlError(ErrorCode::Transaction, "Savepoint does not exist: " + plan.savepointName);
// 保存点不存在时报事务错误。
        buffer_.restoreSavepoint(found->second.file);
        catalog_.restore(found->second.catalog);
// 恢复目录到保存点快照。
        reloadIndexRuntimes();
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
// 返回回滚到保存点成功结果。
    }
// ROLLBACK TO 分支结束。
    if (plan.kind == "Begin") {
// BEGIN 语句。
        if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "Nested transactions are not supported");
// 不支持嵌套事务，已有事务时拒绝。
        buffer_.beginWriteBatch();transaction_ = TransactionState::Active;transactionWriteStatements_ = 0;savepoints_.clear();
// 开启写批次，切换事务为 Active，清零计数和保存点。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
// 返回事务开始成功结果。
    }
// BEGIN 分支结束。
    if (plan.kind == "Commit") {
// COMMIT 语句。
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "No active transaction");
// 没有活动事务时不能提交。
        const auto committedWriteStatements = transactionWriteStatements_;
// 先记录本事务写语句数，供自动检查点评估。
        const auto committedDirtyPages = file_->stagedPageCount();
// 记录本事务产生的脏页数。
        buffer_.commitWriteBatch();transaction_ = TransactionState::Idle;transactionWriteStatements_ = 0;savepoints_.clear();
// 提交写批次，事务回到 Idle，并清空计数和保存点。
        evaluateAutoCheckpoint(committedWriteStatements, committedDirtyPages);
// 根据本事务写入量评估是否触发自动检查点。
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "committed"}};
// 返回提交成功结果。
    }
// COMMIT 分支结束。
    const bool writes = plan.kind == "CreateTable" || plan.kind == "CreateIndex" || plan.kind == "DropIndex" || plan.kind == "Insert" ||
// 判断当前语句是否属于写语句。
                        plan.kind == "Update" || plan.kind == "Delete";
// 建表、建/删索引、插入、更新、删除都算写语句。
    if (transaction_ == TransactionState::Active) {
// 如果已经处于显式事务中。
        auto result = run(plan);
// 直接执行语句，不再为单条语句额外开写批次。
        if (writes) { ++transactionWriteStatements_; invalidateAnalyzeSnapshot(); invalidateRowCountCache(); }
        // 事务内写语句同样会改变行数，两个缓存都要失效。
// 写语句累计事务内写入数，并让 ANALYZE 快照失效。
        return result;
// 返回事务内语句结果。
    }
// 活跃事务分支结束。
    if (!writes) return run(plan);
// 非写语句直接执行，不涉及写批次。
    buffer_.beginWriteBatch();
// 自动提交模式：为这一条写语句开启写批次。
    try {
// 执行写语句；任何异常都要回滚这条语句的写入。
        auto result = run(plan);
// 执行逻辑计划。
        const auto committedDirtyPages = file_->stagedPageCount();
// 记录本条语句产生的脏页数。
        buffer_.commitWriteBatch();
// 提交写批次，使修改对外可见。
        invalidateAnalyzeSnapshot();
        invalidateRowCountCache();
        // 写入已提交：行数可能变了，行数缓存必须一并失效。
// 写成功后让 ANALYZE 快照失效。
        evaluateAutoCheckpoint(1, committedDirtyPages);
// 按一条写语句的写入量评估自动检查点。
        return result;
    } catch (...) {
// 捕获写语句执行期间的异常。
        rollbackBatch();
// 执行回滚，撤销本条语句的部分修改。
        throw;
// 原样重新抛出，让上层看到真实错误。
    }
}
nlohmann::json Database::diagnostics(const std::string& source) const {
// 诊断接口：对整段 SQL 做恢复式词法/语法/语义检查，返回每条语句的结果或错误。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁，保证读取目录和统计时状态稳定。
    requireAvailable();
// 不可用实例拒绝诊断。
    json items = json::array();
// 保存每条语句的诊断条目。
    const auto stageFor = [](ErrorCode code) {
// 把错误码映射成前端阶段名。
        if (code == ErrorCode::Lexical) return "lexer";
// 词法错误归到 lexer 阶段。
        if (code == ErrorCode::Syntax) return "parser";
// 语法错误归到 parser 阶段。
        if (code == ErrorCode::Semantic || code == ErrorCode::Catalog) return "semantic";
// 语义和目录错误归到 semantic 阶段。
        if (code == ErrorCode::NotImplemented) return "planner";
// 未实现功能归到 planner 阶段。
        return "internal";
// 其他错误归到内部错误。
    };
// stageFor lambda 结束。
    std::size_t statementIndex = 0;
// 语句序号从 0 开始，每完成一条自增。
    // 第十七章 REQ-CORE-001：collectDiagnostics 最多 100 条错误。
    // 超出后不再追加错误，但仍在结果里明确报告已截断。
    constexpr std::size_t kMaxDiagnosticErrors = 100;
    std::size_t errorCount = 0;
    bool truncated = false;
    bool budgetReported = false;
    const auto sourceLine = [&](std::size_t line) {
// 从整段源码中截取指定行文本，用于错误上下文展示。
        if (line == 0) return std::string{};
// 行号为 0 表示没有位置信息，返回空串。
        std::size_t current = 1, begin = 0;
// current 记录当前扫描到第几行，begin 是当前行起始下标。
        while (begin <= source.size() && current < line) {
// 逐行推进直到目标行。
            const auto end = source.find('\n', begin);
// 查找下一个换行符。
            if (end == std::string::npos) return std::string{};
// 源文本行数不够，返回空串。
            begin = end + 1;
// 下一行从换行符之后开始。
            ++current;
// 当前行号加一。
        }
// 行推进结束。
        const auto end = source.find('\n', begin);
// 找目标行末尾换行符。
        return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
// 截取该行文本；没有换行则取到结尾。
    };
// sourceLine lambda 结束。
    const auto closestName = [](const std::string& target, const std::vector<std::string>& candidates) {
// 定义按编辑距离找最相近表名/列名的辅助函数。
        if (target.empty()) return std::string{};
// 目标为空时无法给出建议。
        const auto lower = [](std::string value) {
// 定义转小写辅助函数，编辑距离比较大小写不敏感。
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
// 逐字符转小写。
            return value;
// 返回小写结果。
        };
// 小写化 lambda 结束。
        const auto normalizedTarget = lower(target);
// 小写化目标名称。
        std::string best;
// 保存当前最佳候选。
        std::size_t bestDistance = std::numeric_limits<std::size_t>::max();
// 初始化最小编辑距离为最大值。
        for (const auto& candidate : candidates) {
// 遍历所有候选名称。
            const auto normalizedCandidate = lower(candidate);
// 小写化候选项。
            std::vector<std::size_t> previous(normalizedCandidate.size() + 1), current(normalizedCandidate.size() + 1);
// 分配编辑距离动态规划的两行状态。
            for (std::size_t i = 0; i <= normalizedCandidate.size(); ++i) previous[i] = i;
// 初始化空串到候选前缀的距离。
            for (std::size_t i = 1; i <= normalizedTarget.size(); ++i) {
// 逐字符计算目标到候选的编辑距离。
                current[0] = i;
// 当前行第一列表示目标前缀到空串的距离。
                for (std::size_t j = 1; j <= normalizedCandidate.size(); ++j)
// 逐列递推。
                    current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
// 取删除、插入、替换三种操作的最小代价。
                        previous[j - 1] + (normalizedTarget[i - 1] == normalizedCandidate[j - 1] ? 0 : 1)});
// 字符相同则替换代价为 0，否则为 1。
                std::swap(previous, current);
// 交换两行状态，复用空间。
            }
// 行列循环结束。
            const auto distance = previous.back();
// 最终编辑距离在上一行末尾。
            if (distance < bestDistance || (distance == bestDistance && normalizedCandidate < lower(best))) {
// 距离更小，或距离相同但候选字典序更小，就更新最佳。
                bestDistance = distance;
// 保存最佳距离。
                best = candidate;
// 保存最佳候选。
            }
// 最佳候选更新分支结束。
        }
// 候选遍历结束。
        const auto limit = std::max<std::size_t>(1, std::min<std::size_t>(3, normalizedTarget.size() / 2 + 1));
// 根据目标长度设定建议阈值，避免给差距太大的名字。
        return bestDistance <= limit ? best : std::string{};
// 只有距离在阈值内才返回建议。
    };
// closestName lambda 结束。
    const auto append = [&](const MiniSqlError& error) {
// 定义把 MiniSqlError 追加到诊断条目的函数。
        if (errorCount >= kMaxDiagnosticErrors) { truncated = true; return; }
        ++errorCount;
        const auto& loc = error.location();
// 取错误源码位置。
        std::string suggestion = error.suggestion();
// 先取错误对象自带的建议。
        if (suggestion.empty()) {
// 没有建议时根据消息内容推断。
            const std::string message = error.what();
// 取错误消息文本。
            if (message.find("Table does not exist") != std::string::npos || message.find("missing table") != std::string::npos)
// 判断是否是表不存在的错误。
            {
// 进入表名建议分支。
                const auto separator = message.find(':');
// 找消息中的冒号，取表名部分。
                const auto target = message.substr(separator == std::string::npos ? 0 : separator + 1);
// 截取目标表名；没有冒号则取整条消息。
                std::vector<std::string> candidates;
// 收集当前目录中所有表名作为候选。
                for (const auto& table : catalog_.tables()) candidates.push_back(table.definition.table);
// 逐个加入候选。
                const auto matched = closestName(target, candidates);
// 找最相近的表名。
                suggestion = matched.empty() ? "Check the table name and confirm the table was created." : "Did you mean: " + matched + "?";
// 生成表名建议：找到就提示 Did you mean，否则提示检查表名。
// 表名建议分支结束。
            }
// 表名建议分支结束。
            else if (message.find("Column does not exist") != std::string::npos || message.find("Unknown column") != std::string::npos ||
// 判断是否是列不存在的错误。
                     message.find("does not exist in table") != std::string::npos)
// 旧消息也可能提到 does not exist in table 或 unknown column。
            {
// 进入列名建议分支。
                // 新消息形如 Column 'ag' does not exist in table 'student'；旧消息形如 Column does not exist: ag。
// 消息中的列名可能带单引号，也可能用冒号分隔，下面两种格式都尝试解析。
                // 新消息形如 Column 'ag' does not exist in table 'student'；旧消息形如 Column does not exist: ag。
                std::string target;
// 保存从消息中提取的目标列名。
                const auto open = message.find('\'');
// 找第一个单引号。
                const auto close = open == std::string::npos ? std::string::npos : message.find('\'', open + 1);
// 找配对的第二个单引号。
                if (open != std::string::npos && close != std::string::npos) target = message.substr(open + 1, close - open - 1);
// 有引号时取引号之间的列名。
                else {
// 没有引号时退回冒号解析。
                    const auto separator = message.find(':');
// 找冒号分隔符。
                    target = message.substr(separator == std::string::npos ? 0 : separator + 1);
// 取冒号后的列名。
                }
// 引号/冒号解析分支结束。
                std::vector<std::string> candidates;
// 收集目录中所有列名作为候选。
                for (const auto& table : catalog_.tables()) for (const auto& column : table.definition.columns) candidates.push_back(column.name);
// 遍历所有表和列，扁平化列名列表。
                const auto matched = closestName(target, candidates);
// 找最相近的列名。
                suggestion = matched.empty() ? "Check the column name and table alias." : "Did you mean: " + matched + "?";
// 生成列名建议。
            }
// 列名建议分支结束。
            else if (message.find("Expected FROM") != std::string::npos)
// 对缺失 FROM 的语法错误给出针对性建议。
                suggestion = "Add FROM before the table name.";
// 建议在表名前补 FROM。
        }
// 消息推断分支结束。
        items.push_back({{"success", false}, {"stage", stageFor(error.code())},
// 构造一条失败诊断记录。
            {"code", static_cast<int>(error.code())}, {"message", error.what()},
// 错误码转成整数，便于前端稳定判断。
            {"suggestion", std::move(suggestion)}, {"actual", error.actual()},
// 带上建议、实际内容和期望内容。
            {"expected", error.expected()},
// 期望内容字段。
            {"line", loc.line}, {"column", loc.column},
// 起始行与列。
            {"endLine", loc.endLine ? loc.endLine : loc.line},
// 结束行；未提供时退回起始行。
            {"endColumn", loc.endColumn ? loc.endColumn : loc.column},
// 结束列；未提供时退回起始列。
            {"source", sourceLine(loc.line)},
// 截取错误所在源码行。
            {"recoverable", true}, {"statementIndex", statementIndex}});
// 标记可恢复，并记录语句序号。
    };
// append lambda 结束。
    // Tokenize in recovery mode so every lexical error is reported, not only
// 注释：用恢复模式做词法扫描，让每个词法错误都被报告，而不是只报第一个。
    // the first one. Valid tokens still come back for later statements.
// 合法 token 仍会返回，以便继续检查后面的语句。
    // Tokenize in recovery mode so every lexical error is reported, not only
    // the first one. Valid tokens still come back for later statements.
    std::vector<MiniSqlError> lexicalErrors;
// 保存词法错误。
    const auto tokens = sql::tokenizeRecoverable(source, lexicalErrors);
// 调用恢复式分词，内部收集错误而不直接抛异常。
    for (const auto& error : lexicalErrors) append(error);
// 把每个词法错误追加到诊断结果。
    if (!lexicalErrors.empty() && tokens.size() <= 1) {
// 如果词法错误导致只剩 END token，就没有后续语句可分析。
        return {{"success", false}, {"diagnostics", items}, {"count", items.size()},
                {"limit", kMaxDiagnosticErrors}, {"truncated", truncated}};
    }
// 词法错误提前返回分支结束。

    catalog::Catalog snapshot = catalog_.view();
// 复制目录快照，语义检查过程中模拟 DDL 对后续语句的影响。
    std::vector<sql::Token> statement;
// 保存当前一条语句的 token。
    const auto process = [&]() {
// 定义处理一条完整语句的 lambda。
        if (statement.empty()) return;
// 空语句直接返回。
        // 第十七章 REQ-CORE-001：批量语句上限 10000，只报一次预算诊断后停止解析。
        if (statementIndex >= kMaxBatchStatements) {
            if (!budgetReported) {
                budgetReported = true;
                append(MiniSqlError(ErrorCode::Execution, kBatchBudgetMessage, statement.front().location));
            }
            statement.clear();
            return;
        }
        std::vector<MiniSqlError> syntaxErrors;
// 保存语法错误。
        const auto ast = sql::parseRecoverable(statement, syntaxErrors);
// 用恢复模式解析，尽量返回合法语句和全部错误。
        for (const auto& error : syntaxErrors) append(error);
// 追加语法错误到诊断结果。
        for (const auto& item : ast) {
// 遍历解析出的语句。
            if (item.invalid) continue; // offending statement; already reported above
// 已经有语法错误标记的语句跳过语义检查。
            try {
// 对合法语句尝试语义编译。
                const auto nextSnapshot = catalog::compileSnapshot({item}, snapshot);
// 先计算该语句执行后的目录快照。
                (void)sql::compilePlans({item}, snapshot);
// 再尝试编译逻辑计划，验证语义。
                snapshot = nextSnapshot;
// 编译通过后把快照推进到下一状态。
            } catch (const MiniSqlError& error) { append(error); continue; }
// 语义错误追加诊断并继续检查下一条。
            items.push_back({{"success", true}, {"stage", "passed"}, {"kind", item.kind},
// 语义通过时追加成功条目。
                {"line", item.location.line}, {"column", item.location.column},
// 记录语句位置与类型。
                {"endLine", item.location.endLine ? item.location.endLine : item.location.line},
// 记录起始行列。
                {"endColumn", item.location.endColumn ? item.location.endColumn : item.location.column},
// 记录结束行列。
                {"source", sourceLine(item.location.line)},
// 记录源码行。
                {"statementIndex", statementIndex}});
// 记录语句序号。
        }
// 当前语句处理结束。
        ++statementIndex;
// 语句序号加一。
        statement.clear();
// 清空语句 token，准备下一条。
    };
// process lambda 结束。
    for (const auto& token : tokens) {
// 遍历所有 token，按分号切分语句。
        if (token.type == "END") { process(); break; }
// END 表示输入结束，处理最后一条语句后退出。
        statement.push_back(token);
// 把当前 token 加入语句。
        if (token.type == "DELIMITER" && token.lexeme == ";") process();
// 遇到分号分隔符时结束当前语句。
    }
// token 遍历结束。
    const bool success = std::all_of(items.begin(), items.end(), [](const json& item) { return item.value("success", false); });
// 所有条目都成功才算整体成功。
    return {{"success", success}, {"diagnostics", items}, {"count", items.size()},
            {"limit", kMaxDiagnosticErrors}, {"truncated", truncated}};
}
nlohmann::json Database::runCorrelatedSubquery(const json& expression, const json& row) {
// 执行相关子查询：用当前外层行的值绑定子查询引用，并缓存不同参数的结果。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁，保证目录与缓存访问安全。
    const auto sql = expression.at("subquerySql").get<std::string>();
// 取出子查询 SQL 文本。
    const auto& scope = expression.at("outerColumns");
// 取出外层列作用域表，描述子查询引用了哪些外层列。
    OuterBinding outer;
// 构造外层列绑定：别名 -> 列号与类型。
    outer.reserve(scope.size());
// 按作用域大小预留空间。
    for (auto it = scope.begin(); it != scope.end(); ++it)
// 遍历作用域中的每一项。
        outer.emplace(it.key(),
// 插入别名到列信息的映射。
                      std::make_pair(it.value().at("columnId").get<std::size_t>(),
// 列信息包含列号。
                                     it.value().at("type").get<std::string>()));
// 以及列类型。
    // 缓存按 subquerySql 解析的结构化 AST，执行时以 by-value 参数绑定替换外层列，
// 注释：缓存按 subquerySql 解析出的结构化 AST。
    // 避免逐行文本重解析与字面量改写；仍以当前 catalog 编译，保证 schema 变更生效。
// 注释：执行时用 by-value 参数绑定替换外层列，避免逐行文本重解析与字面量改写。
    OuterValues currentValues;
    for (const auto& [name, binding] : outer) {
        if (binding.first >= row.size()) fail("Correlated subquery outer column outside row");
        currentValues.emplace(name, std::make_pair(row.at(binding.first), binding.second));
    }
    // 缓存按 subquerySql 解析的结构化 AST，执行时以 by-value 参数绑定替换外层列，
    // 避免逐行文本重解析与字面量改写；仍以当前 catalog 编译，保证 schema 变更生效。
    auto& ast = correlatedAstCache_[sql];
// 从 AST 缓存取出该子查询语法树。
    if (ast.empty()) ast = sql::parse(sql::tokenize(sql + ";"));
// 首次遇到时解析并缓存。
    if (ast.size() != 1 || ast.front().kind != "Select")
// 相关子查询必须是单条 SELECT。
        throw MiniSqlError(ErrorCode::Semantic, "Correlated subquery must be SELECT");
// 否则抛语义错误。
    // 相关子查询「保守执行优化」：结果仅取决于被引用的外层列绑定值。以
// 注释：相关子查询保守执行优化——结果只取决于被引用的外层列绑定值。
    // (subquerySql|scope) 标识相关形状、以绑定值分组，对每个不同参数物化子查询一次
// 注释：以 SQL 与作用域标识相关形状，再按绑定值分组，参数相同只物化一次。
    // （集合语义半连接），结果在单条语句生命周期内复用，避免对重复参数逐行重执行。
// 注释：结果在单条语句生命周期内复用，避免重复参数逐行重执行。

    // 相关子查询「保守执行优化」：结果仅取决于被引用的外层列绑定值。以
    // (subquerySql|scope) 标识相关形状、以绑定值分组，对每个不同参数物化子查询一次
    // （集合语义半连接），结果在单条语句生命周期内复用，避免对重复参数逐行重执行。
    const std::string prepKey = sql + "\x1f" + scope.dump();
// 生成本次相关形状的缓存前缀键。
    auto& referenced = correlatedColumnsCache_[prepKey];
// 取出该子查询实际引用的外层列号列表。
    if (referenced.empty()) {
// 首次遇到该相关形状时需要分析引用列。
        std::set<std::size_t> ids;
// 用集合收集外层列号，自动去重。
        collectStatementOuterReferences(ast.front(), outer, ids);
// 递归遍历语句，找出真正引用外层绑定的列。
        referenced.assign(ids.begin(), ids.end());
// 把集合转成稳定顺序的列号列表并缓存。
    }
// 引用列分析结束。
    json tuple = json::array();
// 构造本次外层行对应的绑定值元组。
    for (const auto id : referenced) {
// 按引用列列表逐个取值。
        if (id >= row.size()) fail("Correlated subquery outer column outside row");
// 列号越界说明外层行模式与绑定不一致。
        tuple.push_back(row.at(id));
// 把外层列值加入元组。
    }
// 元组构造结束。
    std::map<std::string, json> inheritedTuple;
    for (const auto& frame : activeOuterValues)
        for (const auto& [name, value] : frame) inheritedTuple[name] = value.first;
    for (const auto& [name, value] : inheritedTuple) tuple.push_back({{"name", name}, {"value", value}});
    const std::string fullKey = prepKey + "\x1f" + tuple.dump();
// 用缓存前缀和绑定值生成完整缓存键。
    const auto cached = correlatedRowsCache_.find(fullKey);
// 查询本次绑定值是否已有缓存结果。
    if (cached != correlatedRowsCache_.end()) return cached->second;
// 命中缓存直接返回，无需重新执行子查询。

    sql::Statement bound = bindOuterStatement(ast.front(), outer, row);
// 把外层列替换成当前行值，得到可编译的绑定语句。
    ActiveOuterScope outerScope(std::move(currentValues));
    auto subplans = sql::compilePlans({std::move(bound)}, catalog_.view());
    materializeSubqueries(subplans);
    const auto result = run(subplans.front());
// 执行子查询计划。
    auto rows = result.at("rows");
// 取出结果行。
    correlatedRowsCache_.emplace(std::move(fullKey), rows);
// 把结果写入缓存，供相同绑定值复用。
    return rows;
// 返回子查询结果。
}
void Database::materializeSubqueries(std::vector<sql::LogicalPlan>& plans) {
// 把计划中的非相关子查询预先执行成字面量，并标记相关子查询为运行期处理。
    const auto isCorrelated = [&](const json& expression) {
// 判断表达式是否真的引用了外层列。
        if (!activeOuterValues.empty()) return true;
        if (!expression.contains("outerColumns") || !expression.at("outerColumns").is_object()) return false;
// 没有 outerColumns 描述就不可能是相关子查询。
        const auto tokens = sql::tokenize(expression.at("subquerySql").get<std::string>());
// 对子查询 SQL 做词法分析，寻找“别名.列名”形式的引用。
        for (std::size_t index = 0; index + 2 < tokens.size(); ++index)
// 扫描 token 三元组。
            if (tokens[index].type == "IDENTIFIER" && tokens[index + 1].lexeme == "." && tokens[index + 2].type == "IDENTIFIER" &&
// 形如 IDENTIFIER . IDENTIFIER 时才可能是外层列引用。
                expression.at("outerColumns").contains(key(tokens[index].lexeme + "." + tokens[index + 2].lexeme))) return true;
// 该限定名出现在 outerColumns 中才算相关。
        return false;
// 没有找到相关引用就返回 false。
    };
// isCorrelated lambda 结束。

    struct SubqueryResult { json rows; std::string type; };
// 定义子查询执行结果：结果行和单列类型。
    const auto executeSubquery = [&](const std::string& sql) -> SubqueryResult {
// 定义执行非相关子查询的函数。
        const auto ast = sql::parse(sql::tokenize(sql + ";"));
// 解析子查询 SQL。
        if (ast.size() != 1 || ast.front().kind != "Select")
// 必须是单条 SELECT。
            throw MiniSqlError(ErrorCode::Semantic, "Subquery must be a single SELECT");
// 否则抛语义错误。
        auto subplans = sql::compilePlans(ast, catalog_.view());
        // 内层子查询必须先递归物化。否则嵌套标量子查询的内层会以原始
        // ScalarSubquery 节点进入求值器，在 expression.at("left") 处抛出
        // nlohmann json 异常并泄漏成 InternalError。相关子查询路径同样先物化。
        materializeSubqueries(subplans);
        const auto result = run(subplans.front());
// 执行子查询。
        if (result.at("columns").size() != 1)
// IN/标量子查询必须恰好返回一列。
            throw MiniSqlError(ErrorCode::Semantic, "IN subquery must return exactly one column");
// 列数不符时报语义错误。
        return {result.at("rows"), subplans.front().output.front().type};
// 返回结果行与列类型。
    };
// executeSubquery lambda 结束。
    const auto literalNode = [](const json& value, const std::string& type, const json& origin) {
// 定义把常量包装成 Literal 表达式节点的辅助函数。
        json node = {{"kind", "Literal"}, {"type", value.is_null() ? "null" : type}, {"value", value},
// Literal 节点携带类型、值和源码位置。
            {"line", origin.value("line", 0)}, {"column", origin.value("column", 0)}, {"nullable", value.is_null()}};
// 记录原始行列位置。
        return node;
// 记录是否可空。
    };
// 返回 Literal 节点。
    std::function<json(json)> rewrite;
// 声明递归重写函数；先声明再赋值，是为了在 lambda 内部递归调用自身。
    rewrite = [&](json expression) -> json {
// 定义表达式重写入口：处理子查询节点并递归重写左右孩子。
        if (!expression.is_object()) return expression;
// 非对象表达式没有再往下重写的必要，直接返回。
        const auto kind = expression.value("kind", "");
// 取表达式节点种类。
        if (kind == "Exists") {
// EXISTS 子查询分支。
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedExists"; return expression; }
// 相关 EXISTS 保留到执行期，用 CorrelatedExists 标记。
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
// 非相关 EXISTS 直接执行子查询。
            return literalNode(!result.rows.empty(), "bool", expression);
// EXISTS 转成布尔字面量：有行 true，无行 false。
        }
// EXISTS 分支结束。
        if (kind == "ScalarSubquery") {
// 标量子查询分支。
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedScalarSubquery"; return expression; }
// 相关标量子查询保留到执行期。
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
// 非相关标量子查询直接执行。
            if (result.rows.empty()) return literalNode(nullptr, "null", expression);
// 没有结果行时标量值为 NULL。
            if (result.rows.size() != 1 || !result.rows.front().is_array() || result.rows.front().size() != 1)
// 标量子查询最多只能返回一行一列。
                throw MiniSqlError(ErrorCode::Execution, "Scalar subquery returned more than one row or column");
// 否则抛执行错误。
            return literalNode(result.rows.front().front(), result.type, expression);
// 把唯一的标量值替换成 Literal 节点。
        }
// 标量子查询分支结束。
        if (kind == "InSubquery") {
// IN 子查询分支。
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedInSubquery"; return expression; }
// 相关 IN 子查询保留到执行期。
            auto left = rewrite(expression.at("left"));
// 非相关 IN 子查询先重写左侧表达式。
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
// 执行右侧子查询。
            json combined;
// 保存累积的 OR 表达式。
            bool first = true;
// 标记是否是第一个比较项。
            for (const auto& row : result.rows) {
// 遍历子查询结果的每一行。
                if (!row.is_array() || row.size() != 1) throw MiniSqlError(ErrorCode::Semantic, "IN subquery must return one scalar column");
// IN 子查询结果必须是一行一列。
                const auto& value = row.front();
// 取出标量值。
                json comparison = {{"kind", "Binary"}, {"operator", "="}, {"type", "bool"}, {"nullable", true},
// 构造 left = value 的比较节点。
                    {"left", left}, {"right", literalNode(value, result.type, expression)},
// 右侧写回 Literal 节点。
                    {"line", expression.value("line", 0)}, {"column", expression.value("column", 0)}};
// 复制源码位置。
                if (first) { combined = std::move(comparison); first = false; }
// 第一个比较项直接作为累积结果。
                else combined = {{"kind", "Binary"}, {"operator", "OR"}, {"type", "bool"}, {"nullable", true},
// 后续比较项用 OR 串起来。
                    {"left", std::move(combined)}, {"right", std::move(comparison)},
// OR 的左右孩子。
                    {"line", expression.value("line", 0)}, {"column", expression.value("column", 0)}};
// OR 节点源码位置。
            }
// 子查询结果行遍历结束。
            if (first) return literalNode(false, "bool", expression);
// 子查询为空时 IN 恒为 false。
            return combined;
// 返回展开后的 OR 表达式。
        }
// IN 子查询分支结束。
        if (expression.contains("left") && !expression.at("left").is_null()) expression["left"] = rewrite(expression.at("left"));
// 普通表达式递归重写左孩子。
        if (expression.contains("right") && !expression.at("right").is_null()) expression["right"] = rewrite(expression.at("right"));
// 普通表达式递归重写右孩子。
        return expression;
// 返回重写后的表达式。
    };
// rewrite lambda 结束。
    std::function<void(sql::LogicalPlan&)> visit;
// 声明遍历逻辑计划树的递归函数。
    visit = [&](sql::LogicalPlan& plan) {
// 定义 visit：对计划节点所有表达式字段做重写。
        plan.predicate = rewrite(plan.predicate);
// 重写过滤谓词。
        for (auto& expression : plan.projections) expression = rewrite(expression);
// 重写投影表达式。
        for (auto& expression : plan.groupKeys) expression = rewrite(expression);
// 重写分组键表达式。
        for (auto& aggregate : plan.aggregates) if (!aggregate.at("argument").is_null()) aggregate["argument"] = rewrite(aggregate.at("argument"));
// 重写聚合函数参数。
        for (auto& expression : plan.insertExpressions) expression = rewrite(expression);
// 重写 INSERT 表达式。
        for (auto& row : plan.insertRows) for (auto& expression : row.at("expressions")) expression = rewrite(expression);
// 重写多行 INSERT 的每一行表达式。
        for (auto& child : plan.children) visit(child);
// 递归访问所有孩子节点。
    };
// visit lambda 结束。
    for (auto& plan : plans) visit(plan);
// 对每个顶层计划执行重写。
}
nlohmann::json Database::execute(const std::string& source, bool optimize) {
// 演示用开关：设了 MINISQL_NO_OPTIMIZE 就完全不走优化器，作为对比基线。
// 放在这里而不是调用方，是因为 EXPLAIN 与普通执行都在本函数内部走各自的优化分支。
    if (std::getenv("MINISQL_NO_OPTIMIZE") != nullptr) optimize = false;
    // 关闭标志一旦置上，本函数内所有 optimize 分支（含 EXPLAIN）都会按未优化路径执行。
// 执行整段 SQL 文本：逐条扫描、编译、优化并执行，返回结构化结果。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁，保证整段脚本执行期间状态一致。
    json results = json::array();
// 保存每条语句的结果。
    currentQueryId_ = ++querySequence_;
// 为本次多语句执行分配查询序号。
    QueryResourcesScope resources(activeResources_, std::make_shared<QueryResourceManager>(queryMemoryBytes_, tempDiskBytes_));
    try {
// 整个执行过程用 try 捕获，保证错误也返回结构化 JSON。
        requireAvailable();
// 检查数据库实例可用。
        checkCancelled();
// 检查取消标志。
        ActiveDatabaseScope active(this);
// 设置线程局部当前数据库，供表达式里的相关子查询回查。
        // 按词法语句边界逐条分析，保留已成功语句的提交结果。
// 注释：按词法语句边界逐条执行，已成功语句的结果保留，失败后后面的语句不再执行。
        // 按词法语句边界逐条分析，保留已成功语句的提交结果。
        std::vector<sql::Token> statement;
// 保存当前语句的 token。
        std::size_t batchStatements = 0;
        sql::scanTokens(source, [&](const sql::Token& token) {
// 用词法扫描器逐 token 回调。
            if (token.type == "END") {
// END 表示源码结束。
                if (!statement.empty()) {
// 如果还有最后一条没有分号的语句。
                    statement.push_back(token);
// 补上 END token。
                    (void)sql::parse(statement);
// 立即解析一次，尽早抛出语法错误。
                }
// 最后一条处理结束。
                return;
// END 回调返回。
            }
// END 分支结束。
            statement.push_back(token);
// 把当前 token 加入语句。
            if (token.lexeme != ";" || token.type != "DELIMITER") return;
// 只有分号分隔符才表示一条语句结束。
            if (++batchStatements > kMaxBatchStatements)
                throw MiniSqlError(ErrorCode::Execution, kBatchBudgetMessage, statement.front().location);
            if (key(statement.front().lexeme) == "explain") {
// EXPLAIN 语句在入口层特判，因为它需要额外的计划与统计信息。
                const auto location = statement.front().location;
// 记录 EXPLAIN 关键字位置。
                if (transaction_ == TransactionState::Aborted)
// 已中止事务不允许再执行 EXPLAIN。
                    throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
// 抛出事务错误，提示先 ROLLBACK。
                statement.erase(statement.begin());
// 去掉 EXPLAIN 关键字。
                const bool analyze = !statement.empty() && key(statement.front().lexeme) == "analyze";
// 识别是否还有 ANALYZE 修饰。
                if (analyze) statement.erase(statement.begin());
// 有 ANALYZE 时也去掉。
                const auto target = sql::parse(statement);
// 解析剩余的真正语句。
                if (target.size() != 1 || target.front().kind == "Begin" || target.front().kind == "Commit" || target.front().kind == "Rollback")
// EXPLAIN 只能跟一条数据查询或 DDL，不能跟事务控制语句。
                    throw MiniSqlError(ErrorCode::Syntax, "EXPLAIN requires one data query or data definition statement", location);
// 不符合规则抛语法错误。
                const auto rawPlans = sql::compilePlans(target, catalog_.view());
// 编译原始逻辑计划。
                if (analyze && target.front().kind != "Select")
// EXPLAIN ANALYZE 只允许 SELECT。
                    throw MiniSqlError(ErrorCode::Semantic, "EXPLAIN ANALYZE permits only SELECT", location);
// 否则抛语义错误。
                optimizer::Result optimized;
                // 优化结果容器；下面按是否开启优化决定填什么。
                if (optimize) optimized = optimizer::optimize(rawPlans, optimizerOptions());
                // 开启优化时正常跑优化器；关闭时 optimized 保持为空。
                else { optimized.plans = rawPlans; optimized.converged = true; }
                // 关闭优化时把原始计划当作“优化后计划”，让 EXPLAIN 展示与实际执行一致。
                const auto raw = sql::serializePlans(rawPlans);
// 序列化原始计划，供输出。
                const auto optimizedJson = sql::serializePlans(optimized.plans);
// 序列化优化后计划，供输出。
                const auto tableEstimate = [&](const std::string& name) -> std::pair<double, double> {
// 定义估算单表行数和页数的函数。
                    for (const auto& table : catalog_.tables()) if (key(table.definition.table) == key(name)) {
// 在目录中查找指定表。
                        double rows = 0;
// 统计行数。
                        // 改走行数缓存：原来这里也在全表扫描只为数行数，
                        // 而 EXPLAIN 对每个涉及的表都会调一次，与 cachedTableRows 重复。
                        rows = static_cast<double>(cachedTableRows(table.id, table.definition));
// 行数直接取缓存，不再重扫堆表。
                        return {rows, static_cast<double>(file_->pagesFor(table.id).size())};
// 返回行数与页文件页数。
                    }
// 找到表的分支结束。
                    return {0, 0};
// 表不存在时返回 0 行 0 页。
                };
// tableEstimate lambda 结束。
                const auto statsDocument = statistics();
// 获取统计文档，包含表和列的基数、NULL 比例等。
                std::map<std::string, const json*> statsByTable;
// 建立表名到统计对象的索引。
                for (const auto& table : statsDocument.at("tables")) statsByTable[key(table.at("name").get<std::string>())] = &table;
// 遍历统计文档中的表并建映射。
                const auto columnStat = [&](const std::string& table, std::size_t columnId) -> const json* {
// 定义按表名和列号查找列统计的函数。
                    const auto found = statsByTable.find(key(table));
// 在表映射中查表。
                    if (found == statsByTable.end()) return nullptr;
// 表没有统计时返回空。
// 表没有统计时返回空。
                    for (const auto& column : found->second->at("columns"))
// 遍历该表所有列统计。
                        if (column.at("columnId").get<std::size_t>() == columnId) return &column;
// 列号匹配时返回对应统计对象。
                    return nullptr;
// 没找到列统计时返回空。
                };
// columnStat lambda 结束。
                std::function<double(const json&, const std::string&)> selectivity;
// 声明选择率估算函数：输入谓词和表名，返回 0~1 的选择率。
                selectivity = [&](const json& predicate, const std::string& table) -> double {
// 定义选择率估算递归函数。
                    if (!predicate.is_object()) return 1.0;
// 非对象谓词视为无过滤，选择率为 1。
                    if (predicate.value("kind", "") == "Literal") {
// 字面量谓词直接看真假。
                        const auto& value = predicate.at("value");
// 取出字面量值。
                        if (value.is_null() || value == false) return 0.0;
// NULL 或 false 返回 0，表示不选中任何行。
                        if (value == true) return 1.0;
// true 返回 1，表示全选。
                        return 0.25;
// 其他常量默认按 0.25 估计。
                    }
// 字面量分支结束。
                    const auto op = predicate.value("operator", "");
// 取出运算符。
                    if (op == "AND") return selectivity(predicate.at("left"), table) * selectivity(predicate.at("right"), table);
// AND 的选择率是两侧选择率相乘。
                    if (op == "OR") {
// OR 的选择率用独立事件并集公式。
                        const auto left = selectivity(predicate.at("left"), table), right = selectivity(predicate.at("right"), table);
// 先算左右两侧选择率。
                        return std::min(1.0, left + right - left * right);
// 返回 min(1, left + right - left*right)。
                    }
// OR 分支结束。
                    if (op == "NOT") return 1.0 - selectivity(predicate.at("left"), table);
// NOT 的选择率是 1 减去孩子选择率。
                    if ((op == "IS NULL" || op == "IS NOT NULL") && predicate.contains("left")) {
// IS NULL / IS NOT NULL 可以利用列统计中的 NULL 比例。
                        const auto& operand = predicate.at("left");
// 取操作数。
                        if (operand.value("kind", "") == "Identifier") {
// 只有列引用才能查到列统计。
                            const auto* stat = columnStat(table, operand.at("columnId").get<std::size_t>());
// 查找该列的统计对象。
                            if (stat) {
// 有统计时使用真实 nullRatio。
                                const auto ratio = stat->value("nullRatio", 0.0);
// 读取 NULL 比例。
                                return op == "IS NULL" ? ratio : 1.0 - ratio;
// IS NULL 用 nullRatio，IS NOT NULL 用 1-nullRatio。
                            }
// 统计存在分支结束。
                        }
// 列引用分支结束。
                    }
// IS NULL 分支结束。
                    if (predicate.contains("left") && predicate.contains("right")) {
// 一般二元比较：尝试找列与字面量组合。
                        const auto& left = predicate.at("left");
// 取左操作数。
                        const auto& right = predicate.at("right");
// 取右操作数。
                        const json* id = left.value("kind", "") == "Identifier" ? &left : right.value("kind", "") == "Identifier" ? &right : nullptr;
// 找哪一侧是列引用。
                        const json* literal = left.value("kind", "") == "Literal" ? &left : right.value("kind", "") == "Literal" ? &right : nullptr;
// 找哪一侧是字面量。
                        if (id && literal && id->contains("columnId")) {
// 一侧列一侧字面量时才用 distinctCount 估算。
                            const auto* stat = columnStat(table, id->at("columnId").get<std::size_t>());
// 查找列统计。
                            if (stat && op == "=") {
// 等值比较且统计可用时使用基数。
                                const auto distinct = stat->value("distinctCount", std::size_t{0});
// 读取不同值个数。
                                if (distinct > 0) return 1.0 / static_cast<double>(distinct);
// 选择率约为 1/不同值个数。
                            }
// 等值估算分支结束。
                        }
// 列-字面量分支结束。
                    }
// 一般二元比较分支结束。
                    if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") return 0.33;
// 其他比较运算符默认按 0.33 估计。
                    return 0.25;
// 未知表达式默认按 0.25 估计。
                };
// selectivity lambda 结束。
                std::function<std::pair<double, double>(const sql::LogicalPlan&)> estimate;
// 声明计划代价估算函数：输入计划节点，返回估计行数和估计代价。
                estimate = [&](const sql::LogicalPlan& plan) -> std::pair<double, double> {
// 定义代价估算递归函数。
                    if (plan.kind == "SeqScan") {
// 顺序扫描的代价与表行数和页数有关。
                        const auto [rows, pages] = tableEstimate(plan.table);
// 取表的真实行数和页数。
                        return {rows, rows + pages};
// 扫描代价估算为行数加页数。
                    }
// SeqScan 分支结束。
                    if (plan.children.empty()) {
// 没有孩子的叶子节点。
                        if (plan.kind == "Insert") return {static_cast<double>(std::max(plan.values.size(), plan.insertRows.size())), 1.0};
// INSERT 叶子节点按写入行数估代价。
                        return {0.0, 1.0};
// 其他无孩子节点默认 0 行、代价 1。
                    }
// 叶子节点分支结束。
                    auto child = estimate(plan.children.front());
// 先估算第一个孩子的行数与代价。
                    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
// 过滤类节点。
                        const auto rows = child.first * selectivity(plan.predicate, plan.table);
// 过滤后行数等于孩子行数乘选择率。
                        return {rows, child.second + rows};
// 总代价加上过滤本身的开销。
                    }
// 过滤分支结束。
                    if (plan.kind == "Sort") {
// 排序节点。
                        const auto rows = child.first;
// 排序不改变行数。
                        return {rows, child.second + rows * std::log2(std::max(1.0, rows))};
// 代价加上 O(n log n) 的排序开销。
                    }
// 排序分支结束。
                    if (plan.kind == "Limit") return {plan.limit ? std::min(child.first, static_cast<double>(*plan.limit)) : child.first, child.second};
// LIMIT 减少输出行数，但代价沿用孩子代价。
                    if (plan.kind == "Distinct") return {child.first * 0.5, child.second + child.first * 0.5};
// DISTINCT 默认按行数减半估计，并加上去重开销。
                    if (plan.kind == "Aggregate") return {plan.groupKeys.empty() ? 1.0 : std::min(child.first * 0.1, 1000.0), child.second + child.first};
// 聚合无 GROUP BY 时输出 1 行，否则按孩子行数的 10% 估计。
                    if (plan.kind == "NestedLoopJoin" || plan.kind == "LeftJoin" || plan.kind == "RightJoin" || plan.kind == "FullJoin") {
// 嵌套循环类连接。
                        if (plan.children.size() != 2) return {0.0, child.second};
// 连接必须有两个孩子。
                        const auto right = estimate(plan.children[1]);
// 估算右孩子行数与代价。
                        return {child.first * right.first * 0.1, child.second + right.second + child.first * right.first * 0.1};
// 结果行数按笛卡尔积的 10% 估计，并累加两侧代价。
                    }
// 连接分支结束。
                    return {child.first, child.second + child.first};
// 其他节点默认透传行数并追加简单代价。
                };
// estimate lambda 结束。
                std::vector<const sql::LogicalPlan*> planNodes;
// 保存计划节点的指针列表，索引与序列化 JSON 的 id 对应。
                std::function<void(const sql::LogicalPlan&)> collect;
// 声明递归收集节点指针的函数。
                collect = [&](const sql::LogicalPlan& plan) {
// 定义 collect：深度优先把节点加入列表。
                    planNodes.push_back(&plan);
// 记录当前节点指针。
                    for (const auto& child : plan.children) collect(child);
// 递归收集孩子节点。
                };
// collect lambda 结束。
                for (const auto& plan : (optimize ? optimized.plans : rawPlans)) collect(plan);
// 根据是否开启优化，收集对应计划的所有节点。
                json rows = json::array();
// 结果行数组。
                for (const auto& node : (optimize ? optimizedJson : raw)) {
// 遍历序列化后的计划节点。
                    const auto planIndex = node.at("id").get<std::size_t>();
// 读取节点 id。
                    if (planIndex >= planNodes.size()) fail("EXPLAIN plan node index is invalid");
// id 越界说明序列化与计划树不一致。
                    const auto estimated = estimate(*planNodes[planIndex]);
// 估算对应节点的行数与代价。
                    rows.push_back({node.at("kind"), node.at("detail"), estimated.first, estimated.second, "stats-v1", "table-column-statistics-or-default"});
// 每个计划节点输出一行：类型、细节、估算行数、代价和来源。
                }
// 计划节点遍历结束。
                json accessCandidates = json::array();
// 保存候选访问路径。
                double bestCost = std::numeric_limits<double>::infinity();
// 初始化最小代价为无穷大。
                std::string chosenAccess;
// 保存当前选中的访问路径类型。
                for (const auto* node : planNodes) {
// 遍历所有扫描节点。
                    if (node->kind != "SeqScan" && node->kind != "IndexScan") continue;
// 只比较顺序扫描和索引扫描。
                    const auto candidate = estimate(*node);
// 估算该扫描路径。
                    accessCandidates.push_back({{"kind", node->kind}, {"table", node->table},
// 记录候选路径信息。
                        {"estimatedRows", candidate.first}, {"estimatedCost", candidate.second}});
// 包括估算行数和代价。
                    if (candidate.second < bestCost || (candidate.second == bestCost && (chosenAccess.empty() || node->kind < chosenAccess))) {
// 代价更低时更新最优路径。
                        bestCost = candidate.second;
// 保存新的最低代价。
                        chosenAccess = node->kind;
// 保存新的最优访问路径类型。
                    }
// 更新最优路径分支结束。
                }
// 扫描节点遍历结束。
                json explanation = {{"kind", "Explain"}, {"columns", {"node", "detail", "estimatedRows", "estimatedCost", "estimateSource", "statsSource"}},
// 组装 EXPLAIN 输出：列定义与计划行。
                    {"columnTypes", {"varchar", "varchar", "bigint", "float", "varchar", "varchar"}}, {"rows", rows}, {"affectedRows", 0},
// 列类型、行数据与影响行数。
                    {"plan", raw}, {"optimizedPlan", optimizedJson}, {"optimizationRules", optimized.changes},
// 原始计划、优化后计划和优化改写说明。
                    {"estimatedRowsAvailable", true}, {"costModel", "stats-v1"}, {"costModelVersion", 1},
// 代价模型版本信息。
                    {"deterministicTieBreak", "estimated-cost-then-plan-kind"}, {"candidateAccessPaths", accessCandidates},
// 确定性平局规则与候选访问路径。
                    {"chosenAccessPath", chosenAccess.empty() ? nullptr : json(chosenAccess)}, {"executed", false},
// 最终选中的访问路径；没有扫描节点时为 null。
                    {"commitState", "notApplicable"}};
// 标记 EXPLAIN 不执行语句，提交状态不适用。
                if (analyze) {
// EXPLAIN ANALYZE 需要真实执行并采集实际统计。
                    const auto before = buffer_.stats();
// 记录执行前的缓冲池统计。
                    const auto ioBefore = file_->ioStats();
// 记录执行前的磁盘 IO 统计。
                    auto actualPlans = optimize ? optimized.plans : rawPlans;
// 复制实际要执行的计划。
                    materializeSubqueries(actualPlans);
// 把非相关子查询物化成字面量。
                    correlatedRowsCache_.clear();
// 清空相关子查询缓存，确保统计从零开始。
                    std::vector<json> nodeStatistics;
// 保存逐节点统计。
                    nodeStats_ = &nodeStatistics;
// 让 run 把节点统计写入临时向量。
                    const auto started = std::chrono::steady_clock::now();
// 记录执行开始时间。
                    json actual;
// 保存实际执行结果。
                    try { actual = run(actualPlans.front()); }
// 执行计划。
                    catch (...) { nodeStats_ = nullptr; throw; }
// 执行失败时先解除统计指针，避免悬空。
                    nodeStats_ = nullptr;
// 执行结束，取消统计收集。
                    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
// 计算执行耗时（毫秒）。
                    const auto after = buffer_.stats();
// 记录执行后的缓冲池统计。
                    const auto ioAfter = file_->ioStats();
// 记录执行后的磁盘 IO 统计。
                    explanation["kind"] = "ExplainAnalyze";
// 结果类型改为 ExplainAnalyze。
                    explanation["executed"] = true;
// 标记语句确实执行过。
                    explanation["executionStats"] = {{"scope", "query"}, {"nodeStatisticsAvailable", true}, {"nodeStatistics", nodeStatistics},
// 汇总实际执行统计。
                        {"actualRows", actual.at("rows").size()}, {"durationMs", elapsed}, {"loops", 1},
// 实际行数、耗时和循环次数。
                        {"hits", after.hits - before.hits}, {"misses", after.misses - before.misses},
// 缓冲池命中与未命中增量。
                        {"diskReads", ioAfter.reads - ioBefore.reads}, {"diskWrites", ioAfter.writes - ioBefore.writes},
// 磁盘读写增量。
                        {"diskScope", "database-file-pages-including-header"},
// 说明磁盘统计口径。
                        {"stagedPageReads", after.stagedPageReads - before.stagedPageReads},
// 暂存页读取增量。
                        {"stagedPageWrites", after.stagedPageWrites - before.stagedPageWrites},
// 暂存页写入增量。
                        {"ioErrors", ioAfter.errors - ioBefore.errors}};
// IO 错误增量。
                }
// EXPLAIN ANALYZE 分支结束。
                results.push_back(std::move(explanation));
// 把 EXPLAIN 结果加入总结果列表。
                statement.clear();
// 清空当前语句 token。
                return;
// 返回本次分号语句处理。
            }
// EXPLAIN 特判分支结束。
            // X18: `ANALYZE [TABLE] <name>;` 在入口层特判，不进入 AST/计划契约。
            // 它按需重新扫描一次全库统计，并把刷新时间 + 统计版本 + 表快照持久化到旁路文件，
            // 供 statistics() 跨进程报告 source=analyze（任何写语句成功后删除该文件即失效）。
            // X18: `ANALYZE [TABLE] <name>;` 在入口层特判，不进入 AST/计划契约。
            // 它按需重新扫描一次全库统计，并把刷新时间 + 统计版本 + 表快照持久化到旁路文件，
            // 供 statistics() 跨进程报告 source=analyze（任何写语句成功后删除该文件即失效）。
            if (key(statement.front().lexeme) == "analyze") {
// ANALYZE [TABLE] 语句在入口层特判，不进入普通 AST 计划流程。
                const auto location = statement.front().location;
// 记录 ANALYZE 关键字位置。
                if (transaction_ == TransactionState::Aborted)
// 已中止事务不允许执行 ANALYZE。
                    throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
// 抛出事务错误。
                std::size_t begin = 1;
// 从关键字后开始解析表名。
                std::size_t end = statement.size();
// end 指向语句末尾（分号前）。
                if (end > begin && statement.back().type == "DELIMITER") --end;
// 末尾是分号分隔符时排除它。
                if (begin < end && key(statement.at(begin).lexeme) == "table") ++begin;
// 可选跳过 TABLE 关键字。
                if (end <= begin || begin + 1 != end || statement.at(begin).type != "IDENTIFIER")
// 必须恰好剩下一个标识符作为表名。
                    throw MiniSqlError(ErrorCode::Syntax, "ANALYZE expects a single table name", location);
// 不符合格式抛语法错误。
                const auto tableName = statement.at(begin).lexeme;
// 取出表名。
                if (catalog_.view().find(tableName) == nullptr)
// 表必须存在于目录中。
                    throw MiniSqlError(ErrorCode::Catalog, "Unknown table in ANALYZE: " + tableName, location);
// 表不存在抛目录错误。
                const auto tables = liveTableStatistics();
// 重新扫描全库，生成最新实时统计。
                const auto analyzedAtMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
// 记录本次 ANALYZE 的时间戳。
                    std::chrono::system_clock::now().time_since_epoch()).count());
// 系统时钟转毫秒数。
                const json document = {{"table", tableName}, {"analyzedAtMs", analyzedAtMs},
// 构造要持久化的快照文档。
                    {"version", "stats-v1"}, {"tables", tables}};
// 文档包含表名、时间、统计版本和全部表统计。
                std::ofstream stream(analyzeMetadataPath(), std::ios::binary | std::ios::trunc);
// 以二进制截断方式打开旁路 JSON 文件。
                if (!stream) throw MiniSqlError(ErrorCode::Storage, "Unable to persist ANALYZE statistics", location);
// 打开失败抛存储错误。
                stream << document.dump();
// 写入 JSON 文本。
                json rows = json::array();
// 构造 ANALYZE 的返回行。
                for (const auto& table : tables)
// 在统计结果中查找目标表。
                    if (key(table.at("name").get<std::string>()) == key(tableName))
// 表名匹配时生成返回行。
                        rows.push_back({table.at("name"), table.at("rowCount"), table.at("columns").size(), analyzedAtMs, "stats-v1"});
// 返回行包含表名、行数、列数、时间戳和统计版本。
                results.push_back({{"kind", "Analyze"}, {"table", tableName},
// 把 ANALYZE 结果加入总结果。
                    {"columns", {"table", "rowCount", "columnCount", "analyzedAtMs", "statsVersion"}},
// 声明输出列名。
                    {"columnTypes", {"varchar", "bigint", "bigint", "bigint", "varchar"}},
// 声明输出列类型。
                    {"rows", rows}, {"affectedRows", 0}, {"commitState", "committed"},
// 行数据、影响行数和提交状态。
                    {"source", "analyze"}, {"statsVersion", "stats-v1"}, {"analyzedAtMs", analyzedAtMs}});
// 标记数据来源和统计版本。
                statement.clear();
// 清空当前语句。
                return;
// 返回该分号语句处理。
            }
// ANALYZE 特判分支结束。
            auto ast = sql::parse(statement);
            // 查询结果缓存：只对“单条 SELECT + 事务空闲 + 走优化器”生效。
            // 事务内不缓存：事务里的读取可能看到未提交数据，缓存会把隔离性弄坏；
            // 写语句不缓存：把写操作缓存起来等于把它吞掉。
            std::string resultCacheKey;
            // 缓存键；为空表示本条不参与缓存。
            const bool resultCacheable = resultCacheEnabled_ && optimize &&
            // 开关打开、走优化器、
                transaction_ == TransactionState::Idle && ast.size() == 1 && ast.front().kind == "Select";
                // 事务空闲（即自动提交）、只有一条语句、且是 SELECT。
            if (resultCacheable) {
            // 符合条件才去算键与查缓存。
                resultCacheKey = queryResultCacheKey(statement, optimize);
                // 按归一化语句文本算键。
                const auto cached = queryResultCache_.find(resultCacheKey);
                // 先查缓存。
                if (cached != queryResultCache_.end()) {
                // 命中：直接返回上次结果，跳过编译、优化与执行。
                    ++queryResultCacheHits_;
                    // 命中计数加一。
                    results.push_back(cached->second);
                    // 把缓存的结果当作本次结果。
                    statement.clear();
                    // 清空当前语句 token，准备下一条。
                    return;
                    // 直接结束本条语句的处理。
                }
                // 命中分支结束。
                ++queryResultCacheMisses_;
                // 未命中：记一次未命中，继续走正常流程。
            }
            // 缓存查找结束。
// 普通语句：先解析 AST。
            if (transaction_ == TransactionState::Aborted && ast.front().kind != "Rollback")
// 事务已中止时只允许 ROLLBACK。
                throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
// 抛出事务错误。
            auto plans = sql::compilePlans(ast, catalog_.view());
// 编译逻辑计划。
            if (optimize) plans = optimizer::optimize(plans, optimizerOptions()).plans;
            materializeSubqueries(plans);
// 把非相关子查询物化成字面量。
            for (const auto& plan : plans) recordWorkload(plan);
            // 记录本次语句的谓词负载：供索引建议器累积用户的查询习惯。
            // 它只读计划、不改数据，也不影响执行结果。
            for (const auto& plan : plans) {
// 逐条执行编译出的计划（分号内可能有多条）。
                auto result = runStatement(plan);
// 执行单条语句，处理事务和写批次。
                if (plan.kind == "Commit" || plan.kind == "Rollback")
// 如果是 COMMIT/ROLLBACK，需要回填前面语句的提交状态。
                    for (auto& previous : results) if (previous.value("commitState", "") == "pending")
// 找到前面仍挂起的结果并更新状态。
                        previous["commitState"] = plan.kind == "Commit" ? "committed" : "rolledBack";
// 根据 COMMIT 或 ROLLBACK 改成 committed/rolledBack。
                if (!result.contains("commitState")) result["commitState"] = transaction_ == TransactionState::Active ? "pending" : "committed";
// 结果没有提交状态时，根据事务状态补充。
                result["columnTypes"] = json::array();
// 初始化列类型数组。
                for (const auto& column : plan.output) result["columnTypes"].push_back(column.type);
// 按计划输出模式填写列类型。
                if (maxResultRows_ > 0 && result.contains("rows") && result.at("rows").is_array() && result.at("rows").size() > maxResultRows_)
// 如果单条结果超过最大行数预算，直接报错。
                    throw MiniSqlError(ErrorCode::Execution, "Result row budget exceeded");
// 抛出结果行数超限错误。
                if (resultCacheable && !resultCacheKey.empty() && result.contains("rows") && result.at("rows").is_array()
                // 回填条件：本条可缓存、键有效、结果里确实有行数组。
                    && result.at("rows").size() <= resultCacheMaxRows_) {
                    // 且结果行数没超过上限：大结果缓存会把内存吃光，得不偿失。
                    queryResultCache_[resultCacheKey] = result;
                    // 把结果原样存起来，下次同样的语句直接命中。
                }
                // 回填结束。
                results.push_back(std::move(result));
// 把本条语句结果加入总结果。
            }
// 当前分号语句的计划执行结束。
            statement.clear();
// 清空 token，准备下一条语句。
        });
// 词法扫描回调结束。
        return {{"success", true}, {"results", results}, {"statements", results.size()}, {"transactionState", transactionState()}};
// 整段 SQL 成功结束，返回所有结果和事务状态。
    } catch (const MiniSqlError& error) {
// 捕获 MiniSqlError 业务错误。
        return executionFailure(error, std::move(results));
// 转换成执行失败响应，并回滚活跃事务。
    } catch (const std::exception& error) {
// 捕获其他标准异常。
        return executionFailure(MiniSqlError(ErrorCode::Internal, std::string("Execution failed: ") + error.what()), std::move(results));
// 包装成内部错误，仍返回结构化响应。
    }
// 异常分支结束。
}
nlohmann::json Database::executionFailure(const MiniSqlError& error, json results) {
// 把执行错误转换成统一响应；若事务活跃则先回滚。
    auto response = error.toJson();
// 把异常对象转成 JSON。
    if (transaction_ == TransactionState::Active) {
// 如果当前有活跃事务。
        transaction_ = TransactionState::Aborted;
// 先把事务标记为中止，拒绝后续语句。
        try {
// 尝试回滚写批次。
            rollbackBatch();
// 执行回滚，撤销未提交修改。
            transactionWriteStatements_ = 0;
// 清零事务内写语句计数。
            for (auto& previous : results) if (previous.value("commitState", "") == "pending") previous["commitState"] = "rolledBack";
// 把前面挂起结果的状态改为 rolledBack。
            response["transactionRolledBack"] = true;
// 在响应中标记事务已回滚。
        } catch (const MiniSqlError& recoveryError) { response = recoveryError.toJson(); }
// 如果回滚本身失败，用新的错误响应覆盖。
    }
// 活跃事务处理结束。
    response["completedStatements"] = results.size();
// 记录已经完成的语句数。
    response["results"] = std::move(results);
// 带上已完成语句的结果。
    response["transactionState"] = transactionState();
// 带上当前事务状态。
    if (unavailable_ || error.code() == ErrorCode::Storage) response["commitState"] = "unknown";
// 存储错误或实例不可用时，提交状态未知。
    return response;
// 返回失败响应。
}
nlohmann::json Database::executeStreaming(const std::string& source,
// 流式执行单条 SELECT/EXPLAIN：逐行回调，避免把大结果全部留在内存。
                                          const std::function<void(const nlohmann::json&)>& emitMeta,
// 元信息回调：返回列名与列类型。
                                          const std::function<bool(const nlohmann::json&)>& emitRow) {
// 行回调：返回 false 表示客户端停止接收。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁。
    currentQueryId_ = ++querySequence_;
    QueryResourcesScope resources(activeResources_, std::make_shared<QueryResourceManager>(queryMemoryBytes_, tempDiskBytes_));
    requireAvailable();
// 检查实例可用。
    checkCancelled();
// 检查取消。
    ActiveDatabaseScope active(this);
// 设置线程局部当前数据库。
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
// 已中止事务拒绝流式查询。
    const auto statements = sql::parse(sql::tokenize(source));
// 解析 SQL。
    if (statements.size() != 1 || (statements.front().kind != "Select" && statements.front().kind != "Explain"))
// 流式接口只接受单条 SELECT 或 EXPLAIN。
        throw MiniSqlError(ErrorCode::InvalidArgument, "Streaming execution accepts one SELECT or EXPLAIN statement");
// 其他语句抛参数错误。
    auto plans = sql::compilePlans(statements, catalog_.view());
// 编译逻辑计划。
    materializeSubqueries(plans);
// 物化非相关子查询。
    correlatedRowsCache_.clear();
// 清空相关子查询缓存。
    const auto& plan = plans.front();
// 取单条计划。
    json columns = json::array(), columnTypes = json::array();
// 准备列名和列类型数组。
    for (const auto& column : plan.output) {
// 遍历输出模式。
        columns.push_back(column.name);
// 收集列名。
        columnTypes.push_back(column.type);
// 收集列类型。
    }
// 输出模式遍历结束。
    if (emitMeta) emitMeta({{"columns", std::move(columns)}, {"columnTypes", std::move(columnTypes)}, {"kind", plan.kind}});
// 先发元信息回调。
    std::size_t emitted = 0;
// 记录已发送行数。
    try {
// 优先尝试行流式路径。
        auto stream = openRowStream(plan);
// 创建行流。
        json row;
// 复用行对象。
        while (stream->next(row)) {
// 逐行读取。
            checkCancelled();
// 每行检查取消。
            if (maxResultRows_ > 0 && emitted >= maxResultRows_)
                throw MiniSqlError(ErrorCode::Execution, "Result row budget exceeded");
            if (emitRow && !emitRow(row)) throw MiniSqlError(ErrorCode::Cancelled, "Streaming client disconnected");
// 行回调返回 false 说明客户端断开，抛取消错误。
            ++emitted;
// 已发送行数加一。
        }
// 行循环结束。
        stream->close();
        const auto usage = stream->resourceUsage();
// 读取流资源使用信息。
        return {{"success", true}, {"rows", emitted}, {"resourceUsage", usage}};
// 返回流式执行成功结果。
    } catch (const MiniSqlError& error) {
// 捕获行流不支持的错误。
        if (error.code() != ErrorCode::InvalidArgument) throw;
// 只有 InvalidArgument 才回退物化；其他错误继续抛。
    }
// try 块结束。
    auto result = run(plan);
// 回退到物化执行。
    for (auto& row : result.at("rows")) {
// 遍历物化结果行。
        checkCancelled();
// 逐行检查取消。
        if (maxResultRows_ > 0 && emitted >= maxResultRows_)
            throw MiniSqlError(ErrorCode::Execution, "Result row budget exceeded");
        if (emitRow && !emitRow(row)) throw MiniSqlError(ErrorCode::Cancelled, "Streaming client disconnected");
// 客户端停止接收时抛取消错误。
        ++emitted;
// 已发送行数加一。
    }
// 物化行遍历结束。
    return {{"success", true}, {"rows", emitted}, {"resourceUsage", result.value("resourceUsage", json::object())}};
// 返回物化执行成功结果。
}
nlohmann::json Database::executeScript(const std::string& source, bool optimize) {
// 执行一段脚本：执行结束后若有未提交事务，自动回滚，模拟会话关闭语义。
    std::lock_guard<std::recursive_mutex> guard(mu_);
// 加数据库全局递归锁。
    auto response = execute(source, optimize);
    // 演示用开关：MINISQL_NO_OPTIMIZE=1 时完全关闭优化器，作为对比基线。

    if (transaction_ != TransactionState::Idle && !unavailable_) {
// 如果脚本结束时事务仍未提交，且实例可用。
        rollbackBatch();transaction_ = TransactionState::Idle;
// 自动回滚写批次，并把事务状态切回 Idle。
        if (response.value("success", false)) {
// 如果原执行本身成功，需要把“未提交自动回滚”作为错误报告给调用方。
            const auto results = response.at("results");
// 保存已经完成语句的结果。
            response = MiniSqlError(ErrorCode::Transaction, "Session ended with an uncommitted transaction; rolled back").toJson();
// 构造事务未提交错误响应。
            response["results"] = results;response["completedStatements"] = results.size();
// 仍把已完成语句结果和数量带回。
        }
// 成功结果改写分支结束。
        for (auto& result : response["results"]) if (result.value("commitState", "") == "pending") result["commitState"] = "rolledBack";
// 把所有挂起结果标记为已回滚。
        response["transactionState"] = "IDLE";
// 事务状态设为 IDLE。
        response["transactionRolledBack"] = true;
// 标记发生了自动回滚。
    }
// 未提交事务处理结束。
    return response;
// 返回脚本执行响应。
}

nlohmann::json Database::executeSerializedPlan(const nlohmann::json& document) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    json results = json::array();
    currentQueryId_ = ++querySequence_;
    QueryResourcesScope resources(activeResources_, std::make_shared<QueryResourceManager>(queryMemoryBytes_, tempDiskBytes_));
    try {
        requireAvailable();
        checkCancelled();
        ActiveDatabaseScope active(this);
        if (transaction_ == TransactionState::Aborted)
            throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
        const auto plans = sql::deserializePlans(document);
        if (plans.empty())
            throw MiniSqlError(ErrorCode::InvalidArgument, "Serialized plan document contains no plan nodes");
        // 第十七章 REQ-CORE-002：计划绑定编译期的 Catalog 指纹，执行前重新校验。
        // 不一致时拒绝执行并返回 PLAN_STALE_SCHEMA，绝不按旧列偏移访问新数据。
        const auto current = catalog_.view().schemaFingerprint();
        const auto verify = [&](const sql::LogicalPlan& plan) {
            std::vector<const sql::LogicalPlan*> pending{&plan};
            while (!pending.empty()) {
                const auto* node = pending.back();
                pending.pop_back();
                if (!node->catalogFingerprint.empty() && node->catalogFingerprint != current)
                    throw MiniSqlError(ErrorCode::PlanStaleSchema,
                        "PLAN_STALE_SCHEMA: plan was compiled against catalog fingerprint " + node->catalogFingerprint +
                        ", but the current catalog fingerprint is " + current + "; recompile the statement");
                for (const auto& child : node->children) pending.push_back(&child);
            }
        };
        for (const auto& plan : plans) verify(plan);
        for (const auto& plan : plans) {
            checkCancelled();
            auto result = runStatement(plan);
            if (!result.contains("commitState")) result["commitState"] = transaction_ == TransactionState::Active ? "pending" : "committed";
            result["columnTypes"] = json::array();
            for (const auto& column : plan.output) result["columnTypes"].push_back(column.type);
            if (maxResultRows_ > 0 && result.contains("rows") && result.at("rows").is_array() && result.at("rows").size() > maxResultRows_)
                throw MiniSqlError(ErrorCode::Execution, "Result row budget exceeded");
            results.push_back(std::move(result));
        }
        return {{"success", true}, {"results", results}, {"statements", results.size()}, {"transactionState", transactionState()}};
    } catch (const MiniSqlError& error) {
        return executionFailure(error, std::move(results));
    } catch (const std::exception& error) {
        return executionFailure(MiniSqlError(ErrorCode::Internal, std::string("Execution failed: ") + error.what()), std::move(results));
    }
}
// 关闭 minisql 命名空间。
}
