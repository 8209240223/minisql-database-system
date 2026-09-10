#include "minisql/execution/database.hpp"
#include "minisql/optimizer/optimizer.hpp"
#include "minisql/common/arithmetic.hpp"
#include "minisql/common/cast.hpp"
#include "minisql/common/decimal.hpp"
#include "minisql/common/float.hpp"
#include "minisql/storage/bplus_tree.hpp"
#include "minisql/storage/page_bplus_tree.hpp"
#include "minisql/storage/heap.hpp"
#include "minisql/execution/external_sort.hpp"
#include "minisql/sql/serialization.hpp"
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
#include <functional>
#include <limits>

namespace minisql::execution {
namespace {
using json = nlohmann::json;
thread_local Database* activeDatabase = nullptr;
struct ActiveDatabaseScope {
    explicit ActiveDatabaseScope(Database* database) : previous(activeDatabase) { activeDatabase = database; }
    ~ActiveDatabaseScope() { activeDatabase = previous; }
    Database* previous;
};
std::string key(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
bool sqlIdentifier(const std::string& value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}
std::vector<std::string> lexicalAccessObjects(const std::vector<sql::Token>& tokens) {
    std::vector<std::string> words;
    words.reserve(tokens.size());
    for (const auto& token : tokens) {
        if (token.type == "END") break;
        if (token.type == "STRING") continue;
        words.push_back(key(token.lexeme));
    }
    std::unordered_set<std::string> ctes;
    if (!words.empty() && words.front() == "with") {
        std::size_t cursor = words.size() > 1 && words[1] == "recursive" ? 2 : 1;
        for (;;) {
            if (cursor >= words.size() || !sqlIdentifier(words[cursor])) break;
            ctes.insert(words[cursor++]);
            if (cursor < words.size() && words[cursor] == "(") {
                std::size_t columnDepth = 1;
                ++cursor;
                while (cursor < words.size() && columnDepth > 0) {
                    if (words[cursor] == "(") ++columnDepth;
                    else if (words[cursor] == ")") --columnDepth;
                    ++cursor;
                }
            }
            if (cursor + 1 >= words.size() || words[cursor] != "as" || words[cursor + 1] != "(") break;
            cursor += 2;
            std::size_t depth = 1;
            while (cursor < words.size() && depth > 0) {
                if (words[cursor] == "(") ++depth;
                else if (words[cursor] == ")") --depth;
                ++cursor;
            }
            if (cursor >= words.size() || words[cursor] != ",") break;
            ++cursor;
        }
    }
    std::string command;
    if (!words.empty()) {
        command = words.front();
        if (command == "explain") {
            const auto found = std::find_if(words.begin(), words.end(), [](const std::string& value) {
                return value == "select" || value == "insert" || value == "update" || value == "delete";
            });
            if (found != words.end()) command = *found;
        }
    }
    std::vector<std::string> result;
    const auto add = [&](const std::string& value) {
        if (sqlIdentifier(value) && !ctes.contains(value) &&
            std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
    };
    const auto addAfter = [&](std::size_t index, bool allowParenthesized) {
        auto cursor = index + 1;
        if (allowParenthesized && cursor < words.size() && words[cursor] == "(") return;
        if (cursor < words.size() && words[cursor] == "lateral") ++cursor;
        if (cursor < words.size()) add(words[cursor]);
    };
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto& word = words[index];
        if (word == "from" || word == "join" || word == "into" || word == "update" || word == "references")
            addAfter(index, true);
        if (command == "drop" && (word == "table" || word == "on")) addAfter(index, false);
        if (command == "create" && (word == "table" || word == "on")) addAfter(index, false);
    }
    return result;
}
json cell(const storage::Value& value) {
    return std::visit([](const auto& v) -> json {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::monostate>) return nullptr;
        else return json(v);
    }, value);
}
bool accepted(const json& value) { return value.is_boolean() && value.get<bool>(); }
bool hashJoinKeys(const json& predicate, std::size_t leftSize, std::size_t& leftKey, std::size_t& rightKey) {
    if (!predicate.is_object() || predicate.value("kind", "") != "Binary" || predicate.value("operator", "") != "=") return false;
    const auto& left = predicate.at("left");
    const auto& right = predicate.at("right");
    if (!left.is_object() || !right.is_object() || left.value("kind", "") != "Identifier" || right.value("kind", "") != "Identifier") return false;
    const auto a = left.at("columnId").get<std::size_t>();
    const auto b = right.at("columnId").get<std::size_t>();
    if (a < leftSize && b >= leftSize) { leftKey = a; rightKey = b - leftSize; return true; }
    if (b < leftSize && a >= leftSize) { leftKey = b; rightKey = a - leftSize; return true; }
    return false;
}
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Execution, message); }
json cell(const json& value) { return value; }
template <typename Row>
json rowJson(const Row& row) {
    if constexpr (std::is_same_v<std::decay_t<Row>, json>) return row;
    json result = json::array();
    for (const auto& value : row) result.push_back(cell(value));
    return result;
}
ExactDecimal decimalValue(const json& value, const std::string& type) {
    const auto decimal = decimalType(type);
    return decimal ? ExactDecimal::parse(value.get<std::string>(), decimal->precision, decimal->scale) : ExactDecimal::fromInteger(value.get<std::int64_t>());
}
// X09 3.5: 相关子查询 by-value 参数绑定执行。序列化外层列绑定值为 SQL 字面量
// 文本（与解析器产出的 Literal 一致），随后绑定进已缓存的结构化 AST，取代原
// 先“文本重解析 + 字面量改写”的路径。
using OuterBinding = std::unordered_map<std::string, std::pair<std::size_t, std::string>>;
std::string parameterLiteral(const json& value, const std::string& type) {
    if (value.is_null()) return "NULL";
    if (type == "bool") return value.get<bool>() ? "TRUE" : "FALSE";
    if (type == "float") return formatFiniteFloat(value.get<double>());
    if (type == "int" || type == "bigint") return std::to_string(value.get<std::int64_t>());
    if (decimalType(type)) return value.is_string() ? value.get<std::string>() : std::to_string(value.get<std::int64_t>());
    const auto text = value.get<std::string>();
    std::string quoted;
    for (const char ch : text) { quoted += ch; if (ch == '\'') quoted += '\''; }
    if (type == "date") return "DATE'" + quoted + "'";
    return "'" + quoted + "'";
}
std::shared_ptr<sql::Expr> bindOuter(const std::shared_ptr<sql::Expr>& expression, const OuterBinding& outer, const json& row, std::size_t depth = 0) {
    if (!expression) return nullptr;
    if (depth > 256) fail("correlated subquery expression depth exceeded");
    if (expression->kind == "Identifier") {
        const auto found = outer.find(key(expression->value));
        if (found != outer.end()) {
            const auto columnId = found->second.first;
            if (columnId >= row.size()) fail("Correlated subquery outer column outside row");
            auto literal = std::make_shared<sql::Expr>();
            literal->kind = "Literal";
            literal->value = parameterLiteral(row.at(columnId), found->second.second);
            literal->location = expression->location;
            return literal;
        }
    }
    auto cloned = std::make_shared<sql::Expr>();
    cloned->kind = expression->kind;
    cloned->value = expression->value;
    cloned->location = expression->location;
    cloned->subquerySql = expression->subquerySql;
    if (expression->left) cloned->left = bindOuter(expression->left, outer, row, depth + 1);
    if (expression->right) cloned->right = bindOuter(expression->right, outer, row, depth + 1);
    return cloned;
}
sql::Statement bindOuterStatement(const sql::Statement& statement, const OuterBinding& outer, const json& row) {
    sql::Statement out = statement;
    out.where = bindOuter(statement.where, outer, row);
    for (auto& item : out.selectItems) item.expression = bindOuter(item.expression, outer, row);
    for (auto& item : out.orderBy) item.expression = bindOuter(item.expression, outer, row);
    for (auto& assignment : out.assignments) assignment.expression = bindOuter(assignment.expression, outer, row);
    for (auto& check : out.checks) check = bindOuter(check, outer, row);
    for (auto& value : out.valueExpressions) value = bindOuter(value, outer, row);
    for (auto& valueRow : out.valueRows) for (auto& value : valueRow) value = bindOuter(value, outer, row);
    for (auto& column : out.groupBy) column = bindOuter(column, outer, row);
    out.having = bindOuter(statement.having, outer, row);
    for (auto& join : out.joins) join.on = bindOuter(join.on, outer, row);
    if (statement.fromSubquery)
        out.fromSubquery = std::make_shared<sql::Statement>(bindOuterStatement(*statement.fromSubquery, outer, row));
    return out;
}
// X09 3.4: 收集相关子查询 AST 中实际引用到的外层列 columnId（去重、升序），
// 用于按绑定参数分组建缓存键。遍历字段与 bindOuterStatement 对齐。
void collectOuterReferences(const std::shared_ptr<sql::Expr>& expression, const OuterBinding& outer, std::set<std::size_t>& ids) {
    if (!expression) return;
    if (expression->kind == "Identifier") {
        const auto found = outer.find(key(expression->value));
        if (found != outer.end()) ids.insert(found->second.first);
    }
    if (expression->left) collectOuterReferences(expression->left, outer, ids);
    if (expression->right) collectOuterReferences(expression->right, outer, ids);
}
void collectStatementOuterReferences(const sql::Statement& statement, const OuterBinding& outer, std::set<std::size_t>& ids) {
    collectOuterReferences(statement.where, outer, ids);
    for (const auto& item : statement.selectItems) collectOuterReferences(item.expression, outer, ids);
    for (const auto& item : statement.orderBy) collectOuterReferences(item.expression, outer, ids);
    for (const auto& assignment : statement.assignments) collectOuterReferences(assignment.expression, outer, ids);
    for (const auto& check : statement.checks) collectOuterReferences(check, outer, ids);
    for (const auto& value : statement.valueExpressions) collectOuterReferences(value, outer, ids);
    for (const auto& valueRow : statement.valueRows) for (const auto& value : valueRow) collectOuterReferences(value, outer, ids);
    for (const auto& column : statement.groupBy) collectOuterReferences(column, outer, ids);
    collectOuterReferences(statement.having, outer, ids);
    for (const auto& join : statement.joins) collectOuterReferences(join.on, outer, ids);
    if (statement.fromSubquery) collectStatementOuterReferences(*statement.fromSubquery, outer, ids);
}
std::string storedDecimal(const json& value, const storage::ColumnSchema& column, SourceLocation location) {
    try {
        const auto text = value.is_number_integer() ? std::to_string(value.get<std::int64_t>()) : value.get<std::string>();
        return ExactDecimal::parse(text, column.precision, column.scale).format();
    } catch (const MiniSqlError& error) { throw MiniSqlError(error.code(), error.what(), location); }
}
storage::Value indexValue(const json& value, const std::string& type) {
    if (value.is_null()) return std::monostate{};
    if (type == "int") return value.get<std::int32_t>();
    if (type == "bigint") return value.get<std::int64_t>();
    if (type == "float") return requireFiniteFloat(value.get<double>());
    if (type == "bool") return value.get<bool>();
    return value.get<std::string>();
}
double storedFloat(const json& value, SourceLocation location) {
    if (value.is_number_float()) return requireFiniteFloat(value.get<double>(), location);
    if (value.is_number_integer()) return requireFiniteFloat(static_cast<double>(value.get<std::int64_t>()), location);
    if (value.is_string()) return parseFiniteFloat(value.get_ref<const std::string&>(), ErrorCode::Execution, location);
    throw MiniSqlError(ErrorCode::Execution, "FLOAT column requires a numeric value", location);
}
template <typename Row>
json evaluate(const json& expression, const Row& row) {
    const auto kind = expression.at("kind").get<std::string>();
    if (kind == "Literal") return expression.at("value");
    if (kind == "Identifier") {
        const auto index = expression.at("columnId").get<std::size_t>();
        if (index >= row.size()) fail("Plan column outside row");
        return cell(row[index]);
    }
    if (kind == "CorrelatedExists") {
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        return !activeDatabase->runCorrelatedSubquery(expression, rowJson(row)).empty();
    }
    if (kind == "CorrelatedScalarSubquery") {
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        const auto rows = activeDatabase->runCorrelatedSubquery(expression, rowJson(row));
        if (rows.empty()) return nullptr;
        if (rows.size() != 1 || !rows.front().is_array() || rows.front().size() != 1)
            fail("Correlated scalar subquery returned more than one row or column");
        return rows.front().front();
    }
    if (kind == "CorrelatedInSubquery") {
        if (!activeDatabase) fail("Correlated subquery executed outside a database context");
        const auto left = evaluate(expression.at("left"), row);
        if (left.is_null()) return nullptr;
        const auto rows = activeDatabase->runCorrelatedSubquery(expression, rowJson(row));
        bool hasNull = false;
        for (const auto& candidate : rows) {
            if (!candidate.is_array() || candidate.size() != 1) fail("Correlated IN subquery must return one column");
            if (candidate.front().is_null()) hasNull = true;
            else if (candidate.front() == left) return true;
        }
        return hasNull ? json(nullptr) : json(false);
    }
    const json left = evaluate(expression.at("left"), row);
    const SourceLocation location{expression.value("line", std::size_t(0)), expression.value("column", std::size_t(0))};
    if (kind == "Cast") {
        if (decimalType(expression.at("type").get<std::string>())) return castValue(left, expression.at("type").get<std::string>(), location);
        if (!left.is_null() && decimalType(expression.at("left").at("type").get<std::string>())) {
            if (stringType(expression.at("type").get<std::string>())) return castValue(left,expression.at("type").get<std::string>(),location);
            return castValue(ExactDecimal::parse(left.get<std::string>(), 38, 0, true).format(), expression.at("type").get<std::string>(), location);
        }
        return castValue(left, expression.at("type").get<std::string>(), location);
    }
    const auto op = expression.at("operator").get<std::string>();
    if (op == "IS NULL") return left.is_null();
    if (op == "IS NOT NULL") return !left.is_null();
    if (kind == "Unary" && (op == "+" || op == "-") && decimalType(expression.at("type").get<std::string>())) {
        if (left.is_null()) return nullptr;
        const auto decimal = decimalValue(left, expression.at("type").get<std::string>());
        return (op == "-" ? decimal.negated() : decimal).format();
    }
    if (kind == "Unary" && (op == "+" || op == "-") && expression.at("type") == "float") {
        if (left.is_null()) return nullptr;
        const auto number = requireFiniteFloat(left.get<double>(), location);
        return requireFiniteFloat(op == "-" ? -number : number, location);
    }
    if (kind == "Unary" && (op == "+" || op == "-"))
        return left.is_null() ? json(nullptr) : expression.at("type") == "bigint" ? json(arithmetic64(op, 0, left.get<std::int64_t>(), location)) : json(arithmetic(op, 0, left.get<std::int32_t>(), location));
    if (op == "NOT") return left.is_null() ? json(nullptr) : json(!left.get<bool>());
    if (op == "AND" && left == false) return false;
    if (op == "OR" && left == true) return true;
    const json right = evaluate(expression.at("right"), row);
    if (op == "AND" || op == "OR") {
        if (op == "AND" && right == false) return false;
        if (op == "OR" && right == true) return true;
        if (left.is_null() || right.is_null()) return nullptr;
        return right.get<bool>();
    }
    if (left.is_null() || right.is_null()) return nullptr;
    const bool leftFloat = expression.at("left").at("type") == "float";
    const bool rightFloat = expression.at("right").at("type") == "float";
    if (leftFloat || rightFloat) {
        const auto a = requireFiniteFloat(left.get<double>(), location);
        const auto b = requireFiniteFloat(right.get<double>(), location);
        if (isArithmetic(op)) {
            if (op == "+") return requireFiniteFloat(a + b, location);
            if (op == "-") return requireFiniteFloat(a - b, location);
            if (op == "*") return requireFiniteFloat(a * b, location);
            if (op == "/") return requireFiniteFloat(a / b, location);
        }
        if (op == "=") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        fail("Unsupported FLOAT operator");
    }
    if (decimalType(expression.at("left").at("type").get<std::string>()) || decimalType(expression.at("right").at("type").get<std::string>())) {
        const auto a = decimalValue(left, expression.at("left").at("type").get<std::string>());
        const auto b = decimalValue(right, expression.at("right").at("type").get<std::string>());
        if (isArithmetic(op)) return a.arithmetic(op, b, location).format();
        const auto order = a.compare(b);
        if (op == "=") return order == 0;
        if (op == "!=") return order != 0;
        if (op == "<") return order < 0;
        if (op == "<=") return order <= 0;
        if (op == ">") return order > 0;
        if (op == ">=") return order >= 0;
        fail("Unsupported DECIMAL operator");
    }
    if (isArithmetic(op)) return expression.at("type") == "bigint" ? json(arithmetic64(op, left.get<std::int64_t>(), right.get<std::int64_t>(), location)) : json(arithmetic(op, left.get<std::int32_t>(), right.get<std::int32_t>(), location));
    if (op == "=") return left == right;
    if (op == "!=") return left != right;
    if (op == "<") return left < right;
    if (op == "<=") return left <= right;
    if (op == ">") return left > right;
    if (op == ">=") return left >= right;
    fail("Unknown expression operator");
}
storage::RowSchema rowSchema(const sql::Statement& definition) {
    storage::RowSchema result;
    for (const auto& c : definition.columns) {
        if (key(c.type) == "int") result.push_back(storage::ColumnType::Int);
        else if (key(c.type) == "bigint") result.push_back(storage::ColumnType::Bigint);
        else if (key(c.type) == "varchar") result.push_back(storage::ColumnType::Varchar);
        else if (const auto limit = varcharLength(key(c.type))) result.push_back(storage::ColumnSchema::varchar(*limit));
        else if (key(c.type) == "bool") result.push_back(storage::ColumnType::Bool);
        else if (key(c.type) == "date") result.push_back(storage::ColumnType::Date);
        else if (key(c.type) == "float") result.push_back(storage::ColumnType::Float);
        else if (const auto decimal = decimalType(key(c.type))) result.emplace_back(decimal->precision, decimal->scale);
        else fail("Unsupported persisted type");
    }
    return result;
}
}
struct Database::RuntimeIndex {
    RuntimeIndex(std::string indexName, std::string tableName, std::vector<std::size_t> columnIndices, bool isUnique, bool usePage)
        : name(std::move(indexName)), table(std::move(tableName)), columns(std::move(columnIndices)), unique(isUnique),
          pageFile(usePage), tree(64, isUnique) {}
    std::string name;
    std::string table;
    std::vector<std::size_t> columns;
    bool unique;
    bool pageFile;
    storage::BPlusTree tree;                                        // memory 引擎
    std::unique_ptr<storage::PageBPlusTree> pageTree{nullptr};      // page-file 引擎
    std::vector<storage::RowRef> search(const storage::IndexKey& key) const {
        return pageFile ? pageTree->search(key) : tree.search(key);
    }
    std::vector<storage::RowRef> range(const std::optional<storage::IndexKey>& lower, bool lowerInclusive,
                                       const std::optional<storage::IndexKey>& upper, bool upperInclusive) const {
        return pageFile ? pageTree->range(lower, lowerInclusive, upper, upperInclusive)
                        : tree.range(lower, lowerInclusive, upper, upperInclusive);
    }
    std::size_t height() const { return pageFile ? pageTree->height() : tree.height(); }
    std::size_t size() const { return pageFile ? pageTree->size() : tree.size(); }
    bool validate() const { return pageFile ? pageTree->validate() : tree.validate(); }
};
Database::Database(const std::filesystem::path& path, std::size_t frames, storage::PageFile::CommitObserver observer)
    : file_(std::make_shared<storage::PageFile>(path, std::move(observer))), buffer_(file_, frames, storage::ReplacementPolicy::LRU),
      heap_(file_, buffer_), catalog_(heap_) {
    lastCheckpointAt_ = std::chrono::steady_clock::now();
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (const char* engine = std::getenv("MINISQL_INDEX_ENGINE"); engine && std::string(engine) == "memory") pageFileIndexes_ = false;
    if (const char* configured = std::getenv("MINISQL_SORT_MEMORY_ROWS")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) sortMemoryRows_ = static_cast<std::size_t>(parsed);
    }
    if (const char* configured = std::getenv("MINISQL_AGGREGATE_MEMORY_ROWS")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) aggregateMemoryRows_ = static_cast<std::size_t>(parsed);
    }
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_WRITES")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) autoCheckpointWrites_ = static_cast<std::size_t>(parsed);
    }
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_WAL_BYTES")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1024ull * 1024ull * 1024ull) autoCheckpointWalBytes_ = parsed;
    }
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_DIRTY_PAGES")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 1000000) autoCheckpointDirtyPages_ = static_cast<std::size_t>(parsed);
    }
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_DIRTY_RATIO")) {
        char* end = nullptr;const auto parsed = std::strtod(configured, &end);
        if (end && *end == '\0' && std::isfinite(parsed) && parsed > 0.0 && parsed <= 1.0) autoCheckpointDirtyRatio_ = parsed;
    }
    if (const char* configured = std::getenv("MINISQL_AUTO_CHECKPOINT_INTERVAL_MS")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 365ull * 24ull * 60ull * 60ull * 1000ull) autoCheckpointIntervalMs_ = parsed;
    }
    if (const char* configured = std::getenv("MINISQL_MAX_RESULT_ROWS")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 10000000) maxResultRows_ = static_cast<std::size_t>(parsed);
    }
    sortTempDirectory_ = std::getenv("MINISQL_TEMP_DIR") ? std::filesystem::path(std::getenv("MINISQL_TEMP_DIR")) : path.parent_path() / ".minisql-sort";
    if (const char* configured = std::getenv("MINISQL_SESSION_ID"); configured && *configured) sessionId_ = configured;
    if (const char* configured = std::getenv("MINISQL_CANCEL_FILE"); configured && *configured) cancelFile_ = configured;
    if (const char* configured = std::getenv("MINISQL_BACKGROUND_CHECKPOINT_MS")) {
        char* end = nullptr;const auto parsed = std::strtoull(configured, &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 3600000) backgroundCheckpointMs_ = static_cast<std::size_t>(parsed);
    }
    for (const auto& table : catalog_.tables()) rebuildIndexes(table.id);
    if (backgroundCheckpointMs_ > 0) {
        scheduler_ = std::thread([this] { backgroundSchedulerLoop(); });
    }
}
Database::~Database() {
    if (backgroundCheckpointMs_ > 0) {
        {
            std::lock_guard<std::mutex> lock(schedulerMutex_);
            schedulerStop_.store(true);
        }
        schedulerCv_.notify_all();
        if (scheduler_.joinable()) scheduler_.join();
    }
}

void Database::synchronizeAccessCatalog(const nlohmann::json& document, std::uint32_t permissionVersion) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    if (transaction_ != TransactionState::Idle)
        throw MiniSqlError(ErrorCode::Transaction, "Cannot synchronize access catalog during an active transaction");
    const auto payload = document.dump();
    const auto existing = catalog_.accessCatalogRecord();
    if (existing && existing->permissionVersion >= permissionVersion) {
        if (existing->permissionVersion == permissionVersion && existing->payload != payload)
            throw MiniSqlError(ErrorCode::Catalog, "Access catalog version conflict");
        return;
    }
    buffer_.beginWriteBatch();
    try {
        catalog_.storeAccessCatalog(permissionVersion, payload);
        buffer_.commitWriteBatch();
        catalog_.reload();
    } catch (...) {
        if (file_->writeBatchActive()) buffer_.rollbackWriteBatch();
        throw;
    }
    ++catalogVersion_;
}

void Database::setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile) {
    sessionId_ = sessionId;
    cancelFile_ = cancelFile;
}
void Database::requireAvailable() const {
    if (unavailable_) throw MiniSqlError(ErrorCode::Storage, "Database unavailable; reopen for recovery");
    file_->requireHealthy();
}
void Database::checkCancelled() const {
    if (cancelFile_.empty()) return;
    std::error_code error;
    const auto cancelled = std::filesystem::exists(cancelFile_, error);
    if (error) throw MiniSqlError(ErrorCode::Storage, "Cannot inspect cancellation token");
    if (cancelled) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
}
nlohmann::json Database::compile(const std::string& source) const {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
    const auto tokens = sql::tokenize(source);
    const auto ast = sql::parse(tokens);
    const auto plans = sql::compilePlans(ast, catalog_.view());
    const auto optimized = optimizer::optimize(plans);
    return {{"success", true}, {"plan", sql::serializePlans(plans)}, {"optimizedPlan", sql::serializePlans(optimized.plans)},
            {"optimizationRules", optimized.changes}, {"statements", ast.size()},
            {"optimizer", {{"iterations", optimized.iterations}, {"converged", optimized.converged},
                           {"diagnostics", optimized.diagnostics}, {"rules", optimizer::ruleDescriptors()}}},
            {"tokens", sql::serializeTokens(tokens)}, {"ast", sql::serializeAst(ast)},
            {"schemaVersion", 1}, {"planKind", "logical"},
            {"stages", {{"lexer", "passed"}, {"parser", "passed"}, {"semantic", "passed"},
                        {"planner", "passed"}, {"optimizer", "passed"}, {"executor", "notRun"}}}};
}

std::vector<std::string> Database::resolveAccessObjects(const std::string& source) const {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    try {
        auto tokens = sql::tokenize(source);
        if (!tokens.empty() && key(tokens.front().lexeme) == "explain") {
            tokens.erase(tokens.begin());
            if (!tokens.empty() && key(tokens.front().lexeme) == "analyze") tokens.erase(tokens.begin());
        }
        const auto statements = sql::parse(tokens);
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        const auto add = [&](const std::string& name) {
            if (name.empty()) return;
            const auto* bound = catalog_.view().find(name);
            const auto resolved = key(bound ? bound->name : name);
            if (seen.insert(resolved).second) result.push_back(resolved);
        };
        std::function<void(const sql::Statement&)> visitStatement;
        std::function<void(const std::shared_ptr<sql::Expr>&)> visitExpression;
        visitExpression = [&](const std::shared_ptr<sql::Expr>& expression) {
            if (!expression) return;
            visitExpression(expression->left);
            visitExpression(expression->right);
            if (expression->subquery) visitStatement(*expression->subquery);
            else if (!expression->subquerySql.empty()) {
                try {
                    auto nestedSql = expression->subquerySql;
                    const auto last = nestedSql.find_last_not_of(" \t\r\n");
                    if (last == std::string::npos || nestedSql[last] != ';') nestedSql += ';';
                    for (const auto& nested : sql::parse(sql::tokenize(nestedSql))) visitStatement(nested);
                } catch (const MiniSqlError&) {
                    // 不完整子查询由入口层保留现有保守对象扫描结果。
                }
            }
        };
        visitStatement = [&](const sql::Statement& statement) {
            if (!statement.fromSubquery && !statement.table.empty()) add(statement.table);
            for (const auto& join : statement.joins) add(join.table);
            for (const auto& foreignKey : statement.foreignKeys) add(foreignKey.table);
            for (const auto& column : statement.columns)
                if (column.references) add(column.references->first);
            if (statement.fromSubquery) visitStatement(*statement.fromSubquery);
            visitExpression(statement.where);
            visitExpression(statement.having);
            for (const auto& item : statement.selectItems) visitExpression(item.expression);
            for (const auto& item : statement.orderBy) visitExpression(item.expression);
            for (const auto& item : statement.assignments) visitExpression(item.expression);
            for (const auto& item : statement.groupBy) visitExpression(item);
            for (const auto& item : statement.checks) visitExpression(item);
            for (const auto& item : statement.valueExpressions) visitExpression(item);
            for (const auto& row : statement.valueRows)
                for (const auto& item : row) visitExpression(item);
            for (const auto& join : statement.joins) visitExpression(join.on);
        };
        for (const auto& statement : statements) visitStatement(statement);
        return result;
    } catch (const MiniSqlError&) {
        std::vector<MiniSqlError> lexicalErrors;
        const auto tokens = sql::tokenizeRecoverable(source, lexicalErrors);
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        for (const auto& object : lexicalAccessObjects(tokens)) {
            const auto* bound = catalog_.view().find(object);
            const auto resolved = key(bound ? bound->name : object);
            if (seen.insert(resolved).second) result.push_back(resolved);
        }
        return result;
    }
}
nlohmann::json Database::catalog() {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    json tables = json::array();
    for (const auto& table : catalog_.tables()) {
        std::uint64_t rowCount = 0;
        heap_.scan(table.id, rowSchema(table.definition), [&](storage::RowRef, const storage::Row&) { ++rowCount; });
        json columns = json::array();
        for (const auto& column : table.definition.columns)
            columns.push_back({{"name", column.name}, {"type", key(column.type)}, {"nullable", column.nullable}, {"primaryKey", column.primaryKey}, {"unique", column.unique},
                {"defaultValue", column.defaultValue ? json(*column.defaultValue) : json(nullptr)}, {"references", sql::serializeReference(column.references)}});
        json indexes = json::array();
        if (const auto* definition = catalog_.view().find(table.definition.table))
            for (const auto& index : definition->indexes) {
                nlohmann::json entry{{"name", index.name}, {"columns", index.columns}, {"unique", index.unique},
                    {"storage", "page-file"}, {"pageCount", 0}, {"height", 1}};
                for (const auto& runtime : indexes_)
                    if (key(runtime->table) == key(table.definition.table) && key(runtime->name) == key(index.name)) {
                        entry["pageCount"] = file_->pagesFor(indexOwnerId(runtime->table, runtime->name)).size();
                        entry["height"] = runtime->height();
                    }
                indexes.push_back(std::move(entry));
            }
        tables.push_back({{"name", table.definition.table}, {"tableId", table.id}, {"columns", columns}, {"indexes", indexes}, {"keys", sql::serializeKeys(table.definition.keys)},
            {"foreignKeys", sql::serializeForeignKeys(sql::allForeignKeys(table.definition))},
            {"checks", sql::serializeChecks(table.definition.checks)},
            {"constraintNames", sql::serializeConstraintNames(table.definition.constraintNames)},
            {"rowCount", rowCount}, {"allocatedPages", file_->pagesFor(table.id).size()}});
    }
    return {{"tables", tables}, {"buffer", bufferStatus()}, {"schemaVersion", catalog_.catalogSchemaVersion()}};
}
nlohmann::json Database::checkpoint() {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "CHECKPOINT requires an idle transaction");
    buffer_.flushAll();
    file_->checkpoint({catalogVersion_, indexVersion_});
    pendingAutoCheckpointWrites_ = 0;
    pendingAutoCheckpointWalBytes_ = 0;
    lastCheckpointAt_ = std::chrono::steady_clock::now();
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return {{"success", true}, {"kind", "Checkpoint"}, {"wal", "truncated"}};
}

class ScanRowStream : public RowStream {
public:
    ScanRowStream(storage::HeapStore& heap, std::uint64_t tableId, storage::RowSchema schema)
        : heap_(heap), tableId_(tableId), schema_(std::move(schema)) {
        refs_ = heap_.refsFor(tableId_);
    }
    bool next(nlohmann::json& row) override {
        if (cancelled_) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        if (cursor_ >= refs_.size()) return false;
        row = rowJson(heap_.read(tableId_, schema_, refs_[cursor_]));
        ++cursor_;
        ++rows_;
        return true;
    }
    void cancel() override { cancelled_ = true; }
    void close() override { closed_ = true; }
    nlohmann::json resourceUsage() const override {
        return {{"kind", "ScanRowStream"}, {"rows", rows_}, {"pending", refs_.size() > cursor_ ? refs_.size() - cursor_ : 0}, {"closed", closed_}};
    }
private:
    storage::HeapStore& heap_;
    std::uint64_t tableId_;
    storage::RowSchema schema_;
    std::vector<storage::RowRef> refs_;
    std::size_t cursor_ = 0;
    std::size_t rows_ = 0;
    bool cancelled_ = false;
    bool closed_ = false;
};

class FilterRowStream : public RowStream {
public:
    FilterRowStream(std::unique_ptr<RowStream> child, std::function<bool(const nlohmann::json&)> predicate)
        : child_(std::move(child)), predicate_(std::move(predicate)) {}
    bool next(nlohmann::json& row) override {
        while (child_->next(row)) if (predicate_(row)) return true;
        return false;
    }
    void cancel() override { cancelled_ = true; child_->cancel(); }
    void close() override { child_->close(); }
    nlohmann::json resourceUsage() const override {
        return {{"kind", "FilterRowStream"}, {"rows", rows_}, {"pending", pending_}, {"cancelled", cancelled_}};
    }
private:
    std::unique_ptr<RowStream> child_;
    std::function<bool(const nlohmann::json&)> predicate_;
    std::size_t rows_ = 0;
    bool pending_ = false;
    bool cancelled_ = false;
};

class ProjectRowStream : public RowStream {
public:
    ProjectRowStream(std::unique_ptr<RowStream> child, std::function<nlohmann::json(const nlohmann::json&)> project)
        : child_(std::move(child)), project_(std::move(project)) {}
    bool next(nlohmann::json& row) override {
        nlohmann::json input;
        if (!child_->next(input)) return false;
        row = project_(input);
        ++rows_;
        return true;
    }
    void cancel() override { child_->cancel(); }
    void close() override { child_->close(); }
    nlohmann::json resourceUsage() const override {
        return {{"kind", "ProjectRowStream"}, {"rows", rows_}};
    }
private:
    std::unique_ptr<RowStream> child_;
    std::function<nlohmann::json(const nlohmann::json&)> project_;
    std::size_t rows_ = 0;
};

class LimitRowStream : public RowStream {
public:
    LimitRowStream(std::unique_ptr<RowStream> child, std::uint64_t offset, std::optional<std::uint64_t> limit)
        : child_(std::move(child)), offset_(offset), limit_(limit) {}
    bool next(nlohmann::json& row) override {
        if (limit_ && emitted_ >= *limit_) return false;
        while (skipped_ < offset_) {
            nlohmann::json discarded;
            if (!child_->next(discarded)) return false;
            ++skipped_;
        }
        if (!child_->next(row)) return false;
        ++emitted_;
        return true;
    }
    void cancel() override { child_->cancel(); }
    void close() override { child_->close(); }
    nlohmann::json resourceUsage() const override {
        return {{"kind", "LimitRowStream"}, {"rows", emitted_}, {"skipped", skipped_}};
    }
private:
    std::unique_ptr<RowStream> child_;
    std::uint64_t offset_ = 0;
    std::optional<std::uint64_t> limit_;
    std::uint64_t skipped_ = 0;
    std::uint64_t emitted_ = 0;
};

class MaterializedRowStream : public RowStream {
public:
    explicit MaterializedRowStream(nlohmann::json rows) : rows_(std::move(rows)) {}
    bool next(nlohmann::json& row) override {
        if (cursor_ >= rows_.size()) return false;
        row = rows_.at(cursor_++);
        return true;
    }
    void cancel() override { cancelled_ = true; }
    void close() override { closed_ = true; }
    nlohmann::json resourceUsage() const override {
        return {{"kind", "MaterializedRowStream"}, {"rows", rows_.size()}, {"emitted", cursor_}, {"closed", closed_}, {"cancelled", cancelled_}};
    }
private:
    nlohmann::json rows_ = nlohmann::json::array();
    std::size_t cursor_ = 0;
    bool cancelled_ = false;
    bool closed_ = false;
};

nlohmann::json Database::createSnapshot(const std::filesystem::path& target) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    file_->copyTo(target);
    const auto& record = file_->checkpointRecord();
    return {{"success", true}, {"kind", "Snapshot"}, {"target", target.string()},
        {"walBytes", file_->walBytes()}, {"walCutoffBytes", record.walCutoffBytes},
        {"committedSequence", record.committedSequence}, {"catalogVersion", record.catalogVersion},
        {"indexVersion", record.indexVersion}};
}
nlohmann::json Database::indexInspect(const std::string& table, const std::string& index) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    const RuntimeIndex* found = nullptr;
    for (const auto& candidate : indexes_)
        if (key(candidate->table) == key(table) && key(candidate->name) == key(index)) { found = candidate.get(); break; }
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index not found: " + index);
    if (!found->pageFile)
        return {{"kind", "IndexInspect"}, {"table", found->table}, {"index", found->name}, {"present", false},
                {"storage", "memory"}, {"message", "page-level structure only available on the page-file engine"}};
    const auto state = found->pageTree->inspect();
    std::vector<nlohmann::json> pages;
    pages.reserve(state.pages.size());
    const auto refJson = [](const storage::PageRef& ref) { return nlohmann::json{{"id", ref.id}, {"generation", ref.generation}}; };
    for (const auto& info : state.pages)
        pages.push_back({{"page", refJson(info.page)}, {"leaf", info.leaf}, {"height", info.height}, {"keyCount", info.keyCount},
                         {"parent", refJson(info.parent)}, {"left", refJson(info.left)}, {"right", refJson(info.right)}});
    nlohmann::json problems = nlohmann::json::array();
    for (const auto& problem : state.problems) problems.push_back(problem);
    return {{"kind", "IndexInspect"}, {"table", found->table}, {"index", found->name},
            {"present", state.present}, {"root", refJson(state.root)}, {"height", state.height},
            {"nodeCount", state.nodeCount}, {"leafCount", state.leafCount}, {"rowCount", state.rowCount},
            {"leafChainLength", state.leafChainLength}, {"rootReachable", state.rootReachable},
            {"leafChainLinked", state.leafChainLinked}, {"parentLinksValid", state.parentLinksValid},
            {"storage", "page-file"}, {"problems", std::move(problems)}, {"pages", std::move(pages)}};
}
void Database::evaluateAutoCheckpoint(std::size_t committedWriteStatements, std::size_t committedDirtyPages) {
    if (committedWriteStatements == 0) return;
    if (pendingAutoCheckpointWrites_ > std::numeric_limits<std::size_t>::max() - committedWriteStatements)
        pendingAutoCheckpointWrites_ = std::numeric_limits<std::size_t>::max();
    else pendingAutoCheckpointWrites_ += committedWriteStatements;
    const auto committedWalBytes = file_->lastCommitWalBytes();
    if (pendingAutoCheckpointWalBytes_ > std::numeric_limits<std::uint64_t>::max() - committedWalBytes)
        pendingAutoCheckpointWalBytes_ = std::numeric_limits<std::uint64_t>::max();
    else pendingAutoCheckpointWalBytes_ += committedWalBytes;
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : std::min(1.0,
        static_cast<double>(committedDirtyPages) / static_cast<double>(buffer_.capacity()));
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpointAt_).count());
    std::vector<std::string> reasons;
    if (autoCheckpointWrites_ > 0 && pendingAutoCheckpointWrites_ >= autoCheckpointWrites_) reasons.push_back("writes");
    if (autoCheckpointWalBytes_ > 0 && pendingAutoCheckpointWalBytes_ >= autoCheckpointWalBytes_) reasons.push_back("wal-bytes");
    if (autoCheckpointDirtyPages_ > 0 && committedDirtyPages >= autoCheckpointDirtyPages_) reasons.push_back("dirty-pages");
    if (autoCheckpointDirtyRatio_ > 0.0 && dirtyRatio >= autoCheckpointDirtyRatio_) reasons.push_back("dirty-ratio");
    if (autoCheckpointIntervalMs_ > 0 && elapsedMs >= autoCheckpointIntervalMs_) reasons.push_back("interval");
    if (reasons.empty()) return;
    buffer_.flushAll();
    file_->checkpoint({catalogVersion_, indexVersion_});
    ++checkpointCount_;
    pendingAutoCheckpointWrites_ = 0;
    pendingAutoCheckpointWalBytes_ = 0;
    lastAutoCheckpointReasons_ = std::move(reasons);
    lastCheckpointAt_ = now;
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    lastAutoCheckpointAtMs_ = lastCheckpointAtMs_;
}
void Database::evaluateBackgroundCheckpoint() {
    if (unavailable_) return;
    std::vector<std::string> reasons;
    const auto dirtyPages = buffer_.dirtyPages();
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : std::min(1.0, static_cast<double>(dirtyPages) / static_cast<double>(buffer_.capacity()));
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheckpointAt_).count());
    if (autoCheckpointWrites_ > 0 && pendingAutoCheckpointWrites_ >= autoCheckpointWrites_) reasons.push_back("writes");
    if (autoCheckpointWalBytes_ > 0 && pendingAutoCheckpointWalBytes_ >= autoCheckpointWalBytes_) reasons.push_back("wal-bytes");
    if (autoCheckpointDirtyPages_ > 0 && dirtyPages >= autoCheckpointDirtyPages_) reasons.push_back("dirty-pages");
    if (autoCheckpointDirtyRatio_ > 0.0 && dirtyRatio >= autoCheckpointDirtyRatio_) reasons.push_back("dirty-ratio");
    if (autoCheckpointIntervalMs_ > 0 && elapsedMs >= autoCheckpointIntervalMs_) reasons.push_back("interval");
    schedulerLastEvaluateMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    if (reasons.empty()) return;
    // 后台线程仅在空闲且事务空闲时执行检查点，绝不在活动事务提交点之前截断未提交日志。
    if (transaction_ != TransactionState::Idle) { schedulerDeferredReasons_ = std::move(reasons); return; }
    buffer_.flushAll();
    file_->checkpoint({catalogVersion_, indexVersion_});
    ++checkpointCount_;
    pendingAutoCheckpointWrites_ = 0;
    pendingAutoCheckpointWalBytes_ = 0;
    lastAutoCheckpointReasons_ = std::move(reasons);
    lastCheckpointAt_ = now;
    lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    lastAutoCheckpointAtMs_ = lastCheckpointAtMs_;
    schedulerLastRunMs_ = lastCheckpointAtMs_;
    schedulerDeferredReasons_.clear();
}
void Database::backgroundSchedulerLoop() {
    std::unique_lock<std::mutex> lock(schedulerMutex_);
    while (true) {
        schedulerCv_.wait_for(lock, std::chrono::milliseconds(backgroundCheckpointMs_),
            [&] { return schedulerStop_.load(); });
        if (schedulerStop_.load()) break;
        std::lock_guard<std::recursive_mutex> dbLock(mu_);
        evaluateBackgroundCheckpoint();
    }
}
std::string Database::tableFingerprint(std::uint64_t tableId) {
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const std::uint8_t* bytes, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) { hash ^= bytes[index];hash *= 1099511628211ULL; }
    };
    const auto schema = rowSchema(stored->definition);
    heap_.scan(tableId, schema, [&](storage::RowRef ref, const storage::Row& row) {
        const auto bytes = storage::encodeRow(row, schema);mix(bytes.data(), bytes.size());
        const std::uint64_t values[] = {ref.page.id, ref.page.generation, ref.slot.slot, ref.slot.generation};
        mix(reinterpret_cast<const std::uint8_t*>(values), sizeof(values));
    });
    char buffer[17]{};std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}
std::uint64_t Database::indexOwnerId(const std::string& table, const std::string& index) const {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const std::uint8_t* bytes, std::size_t size) {
        for (std::size_t position = 0; position < size; ++position) { hash ^= bytes[position];hash *= 1099511628211ULL; }
    };
    mix(reinterpret_cast<const std::uint8_t*>(table.data()), table.size());
    const std::uint8_t separator = 0;
    mix(&separator, 1);
    mix(reinterpret_cast<const std::uint8_t*>(index.data()), index.size());
    return (1ULL << 63) | (hash & ((1ULL << 63) - 1));
}
void Database::clearIndexPages(std::uint64_t owner) {
    const auto pages = file_->pagesFor(owner);
    for (const auto& page : pages) buffer_.release(page);
}
void Database::persistIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint) {
    clearIndexPages(owner);
    const auto bytes = tree.dump(fingerprint);
    constexpr std::size_t chunkSize = 3800;
    const auto total = static_cast<std::uint32_t>((bytes.size() + chunkSize - 1) / chunkSize);
    for (std::uint32_t sequence = 0; sequence < total; ++sequence) {
        const auto begin = static_cast<std::size_t>(sequence) * chunkSize;
        const auto end = std::min(bytes.size(), begin + chunkSize);
        const auto pageRef = buffer_.allocate(owner);
        auto guard = buffer_.get(pageRef);
        std::vector<std::uint8_t> record;
        record.reserve(12 + end - begin);
        const auto append = [&](std::uint64_t value) {
            for (unsigned shift = 0; shift < 8; shift += 8) record.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
        };
        record.insert(record.end(), {'I', 'X', 'P', 'A', 'G', 'E', 0, 0});
        append(sequence);append(total);append(static_cast<std::uint64_t>(end - begin));
        record.insert(record.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        guard.insert(record);
    }
}
bool Database::loadIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint) {
    const auto pages = file_->pagesFor(owner);
    if (pages.empty()) return false;
    std::vector<std::string> chunks;
    for (const auto& page : pages) {
        auto guard = buffer_.get(page);
        const auto slots = guard.page().liveSlots();
        if (slots.size() != 1) return false;
        const auto record = guard.page().read(slots.front());
        if (record.size() < 25 || record[0] != 'I' || record[1] != 'X') return false;
        const auto decode = [&](std::size_t offset) {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 8; shift += 8) value |= static_cast<std::uint64_t>(record[offset + shift]) << shift;
            return value;
        };
        const auto sequence = decode(8), expectedTotal = decode(16), payloadSize = decode(24);
        if (expectedTotal == 0 || payloadSize > 3800 || sequence >= expectedTotal || record.size() != 32 + payloadSize) return false;
        if (chunks.size() <= sequence) chunks.resize(static_cast<std::size_t>(sequence) + 1);
        if (!chunks[static_cast<std::size_t>(sequence)].empty()) return false;
        chunks[static_cast<std::size_t>(sequence)].assign(record.begin() + 32, record.end());
    }
    if (chunks.empty() || std::any_of(chunks.begin(), chunks.end(), [](const std::string& chunk) { return chunk.empty(); })) return false;
    std::string bytes;
    for (auto& chunk : chunks) bytes += chunk;
    try { tree.restore(bytes, fingerprint);return true; }
    catch (const std::exception&) { return false; }
}
void Database::rebuildIndexes(std::uint64_t tableId) {
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    const auto* definition = catalog_.view().find(stored->definition.table);
    if (!definition) throw MiniSqlError(ErrorCode::Catalog, "Index table definition not found");
    indexes_.erase(std::remove_if(indexes_.begin(), indexes_.end(), [&](const auto& index) { return key(index->table) == key(stored->definition.table); }), indexes_.end());
    const auto schema = rowSchema(stored->definition);
    const auto fingerprint = tableFingerprint(tableId);
    for (const auto& index : definition->indexes) {
        std::vector<std::size_t> columns;
        for (const auto& name : index.columns) columns.push_back(catalog::resolveColumnIndex(*definition, name));
        auto runtime = std::make_unique<RuntimeIndex>(index.name, stored->definition.table, columns, index.unique, pageFileIndexes_);
        const auto owner = indexOwnerId(stored->definition.table, index.name);
        if (pageFileIndexes_) {
            // 页级引擎主路径：每次重建清旧页后从堆全量建树，索引页与堆页同属一个
            // PageFile/WAL，随写批次原子落盘/回滚，因而总是与堆一致（无需指纹复用来判断陈旧）。
            clearIndexPages(owner);
            runtime->pageTree = std::make_unique<storage::PageBPlusTree>(file_, buffer_, owner, 64, index.unique);
            if (!runtime->pageTree->create()) throw MiniSqlError(ErrorCode::Catalog, "Failed to create page index: " + index.name);
            heap_.scan(tableId, schema, [&](storage::RowRef ref, const storage::Row& row) {
                storage::IndexKey key;
                for (const auto column : columns) key.values.push_back(row[column]);
                if (!runtime->pageTree->insert(std::move(key), ref)) throw MiniSqlError(ErrorCode::Execution, "UNIQUE index violation: " + index.name);
            });
            indexes_.push_back(std::move(runtime));continue;
        }
        if (!std::getenv("MINISQL_REBUILD_INDEXES") && loadIndexPages(runtime->tree, owner, fingerprint)) {
            indexes_.push_back(std::move(runtime));continue;
        }
        heap_.scan(tableId, schema, [&](storage::RowRef ref, const storage::Row& row) {
            storage::IndexKey key;
            for (const auto column : columns) key.values.push_back(row[column]);
            if (!runtime->tree.insert(std::move(key), ref)) throw MiniSqlError(ErrorCode::Execution, "UNIQUE index violation: " + index.name);
        });
        persistIndexPages(runtime->tree, owner, fingerprint);
        indexes_.push_back(std::move(runtime));
    }
}
void Database::validateUniqueIndexes(std::uint64_t tableId, const storage::Row& row, const std::optional<storage::RowRef>& ignored) {
    const catalog::StoredTable* stored = nullptr;
    for (const auto& table : catalog_.tables()) if (static_cast<std::uint64_t>(table.id) == tableId) { stored = &table; break; }
    if (!stored) throw MiniSqlError(ErrorCode::Catalog, "Index table identity not found");
    for (const auto& index : indexes_) {
        if (key(index->table) != key(stored->definition.table) || !index->unique) continue;
        storage::IndexKey key;
        for (const auto column : index->columns) key.values.push_back(row[column]);
        const auto matches = index->search(key);
        for (const auto& match : matches) {
            if (ignored && match.page.id == ignored->page.id && match.page.generation == ignored->page.generation &&
                match.slot.slot == ignored->slot.slot && match.slot.generation == ignored->slot.generation) continue;
            throw MiniSqlError(ErrorCode::Execution, "UNIQUE index violation: " + index->name);
        }
    }
}
nlohmann::json Database::statistics() {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    json tables = json::array();
    for (const auto& table : catalog_.tables()) {
        const auto schema = rowSchema(table.definition);
        std::vector<std::set<json>> distinct(schema.size());
        std::vector<std::uint64_t> nulls(schema.size(), 0);
        std::vector<std::optional<json>> minimum(schema.size());
        std::vector<std::optional<json>> maximum(schema.size());
        std::vector<std::map<std::string, std::pair<json, std::uint64_t>>> frequencies(schema.size());
        std::uint64_t rowCount = 0;
        heap_.scan(table.id, schema, [&](storage::RowRef, const storage::Row& row) {
            ++rowCount;
            for (std::size_t index = 0; index < row.size(); ++index) {
                if (std::holds_alternative<std::monostate>(row[index])) ++nulls[index];
                else {
                    auto value = cell(row[index]);
                    distinct[index].insert(value);
                    if (!minimum[index] || value < *minimum[index]) minimum[index] = value;
                    if (!maximum[index] || *maximum[index] < value) maximum[index] = value;
                    auto& bucket = frequencies[index][value.dump()];
                    bucket.first = std::move(value);
                    ++bucket.second;
                }
            }
        });
        json columns = json::array();
        for (std::size_t index = 0; index < table.definition.columns.size(); ++index) {
            std::vector<std::pair<std::string, std::pair<json, std::uint64_t>>> ranked(frequencies[index].begin(), frequencies[index].end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
                if (left.second.second != right.second.second) return left.second.second > right.second.second;
                return left.first < right.first;
            });
            if (ranked.size() > 8) ranked.resize(8);
            json histogram = json::array();
            json valueHistogram = json::array();
            for (const auto& [keyValue, entry] : ranked) {
                (void)keyValue;
                histogram.push_back({{"value", entry.first}, {"count", entry.second}});
            }
            if (!distinct[index].empty()) {
                const std::vector<json> ordered(distinct[index].begin(), distinct[index].end());
                const auto bucketCount = std::min<std::size_t>(8, ordered.size());
                for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
                    const auto begin = bucket * ordered.size() / bucketCount;
                    const auto end = (bucket + 1) * ordered.size() / bucketCount;
                    if (begin >= end) continue;
                    const auto& lower = ordered[begin];
                    const auto& upper = ordered[end - 1];
                    std::uint64_t count = 0;
                    for (const auto& [keyValue, entry] : frequencies[index]) {
                        (void)keyValue;
                        if (!(entry.first < lower) && !(upper < entry.first)) count += entry.second;
                    }
                    valueHistogram.push_back({{"lower", lower}, {"upper", upper}, {"count", count}});
                }
            }
            columns.push_back({{"name", table.definition.columns[index].name},
                {"columnId", index}, {"type", key(table.definition.columns[index].type)},
                {"distinctCount", distinct[index].size()},
                {"nullCount", nulls[index]},
                {"nullRatio", rowCount == 0 ? 0.0 : static_cast<double>(nulls[index]) / static_cast<double>(rowCount)},
                {"minValue", minimum[index] ? *minimum[index] : json(nullptr)},
                {"maxValue", maximum[index] ? *maximum[index] : json(nullptr)},
                {"histogram", std::move(histogram)}, {"valueHistogram", std::move(valueHistogram)}});
        }
        json indexes = json::array();
        for (const auto& definition : table.definition.indexes) {
            const RuntimeIndex* runtime = nullptr;
            for (const auto& candidate : indexes_) if (key(candidate->table) == key(table.definition.table) && key(candidate->name) == key(definition.name)) { runtime = candidate.get(); break; }
            indexes.push_back({{"name", definition.name}, {"columns", definition.columns}, {"unique", definition.unique},
                {"entries", runtime ? runtime->size() : 0}, {"height", runtime ? runtime->height() : 1},
                {"pageCount", runtime && runtime->pageFile ? file_->pagesFor(indexOwnerId(table.definition.table, definition.name)).size() : 0},
                {"statsSource", runtime ? (runtime->pageFile ? "page-bplus-tree" : "memory-bplus-tree") : "missing"}});
        }
        tables.push_back({{"name", table.definition.table}, {"tableId", table.id}, {"rowCount", rowCount},
            {"allocatedPages", file_->pagesFor(table.id).size()}, {"columns", columns}, {"indexes", std::move(indexes)}});
    }
    const auto dirtyPages = buffer_.dirtyPages();
    const auto dirtyRatio = buffer_.capacity() == 0 ? 0.0 : static_cast<double>(dirtyPages) / static_cast<double>(buffer_.capacity());
    const auto& record = file_->checkpointRecord();
    const auto refreshedAt = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    return {{"success", true}, {"tables", tables}, {"scope", "table-column-index"}, {"source", "on-demand-scan"},
        {"statisticsVersion", 1}, {"refreshedAt", refreshedAt}, {"histogramBuckets", 8},
        {"checkpointCount", checkpointCount_}, {"autoCheckpointWrites", autoCheckpointWrites_},
        {"autoCheckpointWalBytes", autoCheckpointWalBytes_}, {"autoCheckpointDirtyPages", autoCheckpointDirtyPages_},
        {"autoCheckpointDirtyRatio", autoCheckpointDirtyRatio_}, {"autoCheckpointIntervalMs", autoCheckpointIntervalMs_},
        {"pendingAutoCheckpointWrites", pendingAutoCheckpointWrites_}, {"pendingAutoCheckpointWalBytes", pendingAutoCheckpointWalBytes_},
        {"walBytes", file_->walBytes()}, {"dirtyPages", dirtyPages}, {"dirtyPageRatio", dirtyRatio},
        {"committedSequence", file_->committedSequence()}, {"dirtyWatermark", file_->dirtyWatermark()},
        {"checkpointRecord", {{"present", record.present}, {"walCutoffBytes", record.walCutoffBytes},
            {"dirtyWatermark", record.dirtyWatermark}, {"catalogVersion", record.catalogVersion},
            {"indexVersion", record.indexVersion}, {"committedSequence", record.committedSequence},
            {"timestampMs", record.timestampMs}}},
        {"lastCheckpointAtMs", lastCheckpointAtMs_}, {"lastAutoCheckpointAtMs", lastAutoCheckpointAtMs_},
        {"lastAutoCheckpointReasons", lastAutoCheckpointReasons_},
        {"backgroundScheduler", {{"enabled", backgroundCheckpointMs_ > 0 && scheduler_.joinable()},
            {"intervalMs", backgroundCheckpointMs_}, {"lastEvaluateMs", schedulerLastEvaluateMs_},
            {"lastRunMs", schedulerLastRunMs_}, {"deferredReasons", schedulerDeferredReasons_}}}};
}
nlohmann::json Database::bufferStatus() const {
    const auto& stats = buffer_.stats();
    const auto requests = static_cast<double>(stats.hits) + static_cast<double>(stats.misses);
    json evictions = json::array();
    for (const auto& event : buffer_.evictions()) {
        evictions.push_back({{"sequence", event.sequence},
            {"policy", event.policy == storage::ReplacementPolicy::LRU ? "LRU" : "FIFO"},
            {"pageId", event.page.id}, {"generation", event.page.generation},
            {"dirty", event.dirty}, {"writeBack", event.writeBack}});
    }
    return {{"available", true}, {"scope", "database-instance"},
        {"policy", buffer_.policy() == storage::ReplacementPolicy::LRU ? "LRU" : "FIFO"},
        {"capacity", buffer_.capacity()}, {"residentPages", buffer_.size()},
        {"hits", stats.hits}, {"misses", stats.misses},
        {"hitRate", requests == 0 ? 0 : static_cast<double>(stats.hits) / requests},
        {"diskReads", file_->ioStats().reads}, {"diskWrites", file_->ioStats().writes},
        {"diskScope", "database-file-pages-including-header"},
        {"ioErrors", file_->ioStats().errors}, {"stagedPageReads", stats.stagedPageReads},
        {"stagedPageWrites", stats.stagedPageWrites}, {"evictions", evictions}};
}
 nlohmann::json Database::configureBuffer(const std::string& action) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    if (transaction_ != TransactionState::Idle)
        throw MiniSqlError(ErrorCode::Transaction, "Buffer configuration requires an idle transaction");
    if (action == "LRU") buffer_.setPolicy(storage::ReplacementPolicy::LRU);
    else if (action == "FIFO") buffer_.setPolicy(storage::ReplacementPolicy::FIFO);
    else if (action == "RESET") buffer_.resetStats();
    else throw MiniSqlError(ErrorCode::InvalidArgument, "Expected LRU, FIFO or RESET");
    return {{"tables", json::array()}, {"buffer", bufferStatus()}};
}
std::vector<storage::Row> Database::joinRows(const sql::LogicalPlan& plan) {
    checkCancelled();
    std::vector<storage::Row> rows;
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
        if (plan.children.size() != 1) fail("Join filter requires one child");
        rows = joinRows(plan.children.front());
        for (auto iterator = rows.begin(); iterator != rows.end();) {
            if (!accepted(evaluate(plan.predicate, *iterator))) iterator = rows.erase(iterator);
            else ++iterator;
        }
        return rows;
    }
    if (plan.kind == "IndexScan") {
        const auto result = runNode(plan);
        const auto& indexedRows = result.at("rows");
        rows.reserve(indexedRows.size());
        for (const auto& row : indexedRows) {
            if (!row.is_array() || row.size() != plan.output.size()) fail("IndexScan join row schema mismatch");
            storage::Row converted;
            converted.reserve(row.size());
            for (std::size_t index = 0; index < row.size(); ++index) converted.push_back(indexValue(row[index], plan.output[index].type));
            rows.push_back(std::move(converted));
        }
        return rows;
    }
    if (plan.kind == "SeqScan") {
        const catalog::StoredTable* table = nullptr;
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
        if (!table) fail("Join scan references missing table");
        heap_.scan(table->id, rowSchema(table->definition), [&](storage::RowRef, const storage::Row& row) {
            checkCancelled();
            rows.push_back(row);
        });
        return rows;
    }
    if ((plan.kind != "NestedLoopJoin" && plan.kind != "HashJoin" && plan.kind != "LeftJoin" && plan.kind != "RightJoin" && plan.kind != "FullJoin") || plan.children.size() != 2) fail("Unsupported join input");
    const auto left = joinRows(plan.children[0]);
    if (left.empty() && plan.kind != "RightJoin" && plan.kind != "FullJoin") return rows;
    const auto right = joinRows(plan.children[1]);
    if (plan.kind == "HashJoin") {
        std::size_t leftKey{}, rightKey{};
        if (!hashJoinKeys(plan.predicate, plan.children[0].output.size(), leftKey, rightKey)) fail("HashJoin requires a direct equality key");
        std::unordered_map<std::string, std::vector<std::size_t>> buckets;
        for (std::size_t index = 0; index < right.size(); ++index) {
            checkCancelled();
            if (rightKey >= right[index].size()) fail("HashJoin right key outside row");
            const auto key = cell(right[index][rightKey]);
            if (key.is_null()) continue;
            buckets[key.dump()].push_back(index);
        }
        for (const auto& leftRow : left) {
            checkCancelled();
            if (leftKey >= leftRow.size()) fail("HashJoin left key outside row");
            const auto key = cell(leftRow[leftKey]);
            if (key.is_null()) continue;
            const auto found = buckets.find(key.dump());
            if (found == buckets.end()) continue;
            for (const auto index : found->second) {
                auto combined = leftRow;
                combined.insert(combined.end(), right[index].begin(), right[index].end());
                rows.push_back(std::move(combined));
            }
        }
        return rows;
    }
    std::vector<bool> rightMatched(right.size(), false);
    for (const auto& a : left) {
        checkCancelled();
        bool matched = false;
        for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
            checkCancelled();
            const auto& b = right[rightIndex];
            auto combined = a;
            combined.insert(combined.end(), b.begin(), b.end());
            if (accepted(evaluate(plan.predicate, combined))) { matched = true;rightMatched[rightIndex] = true;rows.push_back(std::move(combined)); }
        }
        if (!matched && (plan.kind == "LeftJoin" || plan.kind == "FullJoin")) {
            auto combined = a;
            combined.resize(a.size() + plan.children[1].output.size(), std::monostate{});
            rows.push_back(std::move(combined));
        }
    }
    if (plan.kind == "RightJoin" || plan.kind == "FullJoin") for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) if (!rightMatched[rightIndex]) {
        storage::Row combined(plan.children[0].output.size(), std::monostate{});
        combined.insert(combined.end(), right[rightIndex].begin(), right[rightIndex].end());
        rows.push_back(std::move(combined));
    }
    return rows;
}
json Database::aggregateRows(const sql::LogicalPlan& plan) {
    if (plan.children.size() != 1) fail("Aggregate requires one child");
    const auto* input = &plan.children.front();
    const json* predicate = nullptr;
    if (input->kind == "Filter" || input->kind == "SemiJoin" || input->kind == "AntiJoin" || input->kind == "Apply") {
        if (input->children.size() != 1) fail("Aggregate filter requires one child");
        predicate = &input->predicate;
        input = &input->children.front();
    }
    json initial = json::array();
    for (const auto& function : plan.aggregates)
        initial.push_back(function.at("function") == "COUNT" ? json(std::int64_t{0}) :
            function.at("function") == "AVG" ? (function.at("argument").at("type") == "float" ?
                json{{"sum", 0.0}, {"count", std::int64_t{0}}} : json{{"sum", "0"}, {"count", std::int64_t{0}}}) : json(nullptr));

    const auto combine = [&](json& state, const json& aggregate, const json& value, SourceLocation location) {
        if (value.is_null()) return;
        const auto function = aggregate.at("function").get<std::string>();
        const auto& argument = aggregate.at("argument");
        json next = state;
        if (function == "COUNT") next = arithmetic64("+", state.get<std::int64_t>(), 1, location);
        else if (function == "SUM") {
            if (const auto type = decimalType(argument.at("type").get<std::string>())) {
                next = state.is_null() ? value : json(ExactDecimal::parse(state.get<std::string>(), 38, type->scale).arithmetic("+", decimalValue(value, type->name()), location).format());
            } else if (argument.at("type") == "float")
                next = state.is_null() ? value : json(requireFiniteFloat(state.get<double>() + value.get<double>(), location));
            else next = state.is_null() ? value : json(arithmetic64("+", state.get<std::int64_t>(), value.get<std::int64_t>(), location));
        } else if (function == "MIN" || function == "MAX") {
            if (next.is_null()) next = value;
            else {
                const auto type = argument.at("type").get<std::string>();
                const auto order = decimalType(type) ? decimalValue(value, type).compare(decimalValue(next, type)) : value < next ? -1 : value > next ? 1 : 0;
                if ((function == "MIN" && order < 0) || (function == "MAX" && order > 0)) next = value;
            }
        } else if (function == "AVG") {
            if (argument.at("type") == "float") {
                next = {{"sum", requireFiniteFloat(state.at("sum").get<double>() + value.get<double>(), location)},
                    {"count", arithmetic64("+", state.at("count").get<std::int64_t>(), 1, location)}};
            } else {
                ExactDecimal::Integer total(state.at("sum").get<std::string>());
                if (decimalType(argument.at("type").get<std::string>())) total += decimalValue(value, argument.at("type").get<std::string>()).coefficient();
                else total += value.get<std::int64_t>();
                next = {{"sum", total.convert_to<std::string>()},
                    {"count", arithmetic64("+", state.at("count").get<std::int64_t>(), 1, location)}};
            }
        } else throw MiniSqlError(ErrorCode::NotImplemented, "Aggregate function evaluation is not implemented: " + function);
        state = std::move(next);
    };

    std::vector<json> records;
    const auto consume = [&](const storage::Row& row) {
        checkCancelled();
        if (predicate && !accepted(evaluate(*predicate, row))) return;
        json groupKey = json::array();
        for (const auto& expression : plan.groupKeys) groupKey.push_back(evaluate(expression, row));
        json values = json::array();
        for (const auto& aggregate : plan.aggregates) {
            const auto& argument = aggregate.at("argument");
            values.push_back(argument.is_null() ? json(true) : evaluate(argument, row));
        }
        records.push_back({{"key", std::move(groupKey)}, {"values", std::move(values)}});
    };
    if (input->kind == "SeqScan") {
        const catalog::StoredTable* table = nullptr;
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(input->table)) table = &candidate;
        if (!table) fail("Aggregate scan references missing table");
        heap_.scan(table->id, rowSchema(table->definition), [&](storage::RowRef, const storage::Row& row) { consume(row); });
    } else if (input->kind == "NestedLoopJoin" || input->kind == "HashJoin" || input->kind == "LeftJoin" || input->kind == "RightJoin" || input->kind == "FullJoin") {
        for (const auto& row : joinRows(*input)) consume(row);
    } else if (input->kind == "Limit" && input->limit && *input->limit == 0) {
        // 恒假过滤已被改写为 Limit 0；空输入仍需保留全局聚合的一行结果。
    } else fail("Unsupported aggregate input");
    if (plan.groupKeys.empty() && records.empty()) {
        json values = json::array();
        for (std::size_t index = 0; index < plan.aggregates.size(); ++index) values.push_back(nullptr);
        records.push_back({{"key", json::array()}, {"values", std::move(values)}});
    }

    const bool external = records.size() > aggregateMemoryRows_;
    const auto compareRecords = [](const json& left, const json& right) { return left.at("key") < right.at("key"); };
    if (external) {
        const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-a" + std::to_string(++aggregateSequence_);
        externalSort(records, compareRecords, aggregateMemoryRows_, sortTempDirectory_, operationId, [this] { checkCancelled(); });
    } else std::stable_sort(records.begin(), records.end(), compareRecords);

    constexpr std::size_t maximumPayload = 64 * 1024 * 1024;
    json rows = json::array();
    json currentKey = json::array();
    json state = initial;
    bool active = false;
    std::size_t groupCount = 0, groupPayloadBytes = 0, totalPayloadBytes = 0;
    const auto emit = [&]() {
        auto row = currentKey;
        for (std::size_t index = 0; index < state.size(); ++index) {
            if (plan.aggregates[index].at("function") != "AVG") row.push_back(state[index]);
            else {
                const auto count = state[index].at("count").get<std::int64_t>();
                if (plan.aggregates[index].at("type") == "float") {
                    const auto& aggregateArgument = plan.aggregates[index].at("argument");
                    row.push_back(count == 0 ? json(nullptr) : json(requireFiniteFloat(state[index].at("sum").get<double>() / static_cast<double>(count), {aggregateArgument.value("line", std::size_t{0}), aggregateArgument.value("column", std::size_t{0})})));
                    continue;
                }
                ExactDecimal::Integer denominator = count;
                const auto argumentType = decimalType(plan.aggregates[index].at("argument").at("type").get<std::string>());
                if (argumentType) for (unsigned digit = 0; digit < argumentType->scale; ++digit) denominator *= 10;
                const auto outputType = decimalType(plan.aggregates[index].at("type").get<std::string>());
                try {
                    row.push_back(count == 0 ? json(nullptr) : json(ExactDecimal::fromRatio(
                        ExactDecimal::Integer(state[index].at("sum").get<std::string>()), denominator, 38, outputType->scale).format()));
                } catch (const MiniSqlError& error) {
                    const auto& argument = plan.aggregates[index].at("argument");
                    throw MiniSqlError(error.code(), error.what(), {argument.value("line", std::size_t{0}), argument.value("column", std::size_t{0})});
                }
            }
        }
        if (row.size() != plan.output.size()) fail("Aggregate output schema mismatch");
        rows.push_back(std::move(row));
    };

    for (const auto& record : records) {
        if (!record.is_object() || !record.contains("key") || !record.contains("values") || !record.at("key").is_array() ||
            !record.at("values").is_array() || record.at("values").size() != plan.aggregates.size())
            fail("Aggregate record schema mismatch");
        if (!active || record.at("key") != currentKey) {
            if (active) emit();
            if (++groupCount > 65536) fail("Aggregate group budget exceeded");
            currentKey = record.at("key");
            state = initial;
            active = true;
            groupPayloadBytes = currentKey.dump().size() + state.dump().size();
            if (!external) {
                if (groupPayloadBytes > maximumPayload - totalPayloadBytes) fail("Aggregate state payload budget exceeded");
                totalPayloadBytes += groupPayloadBytes;
            }
        }
        const auto& values = record.at("values");
        for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
            if (values[index].is_null()) continue;
            const auto& aggregate = plan.aggregates[index];
            const auto& argument = aggregate.at("argument");
            const SourceLocation location{argument.is_null() ? 0 : argument.value("line", std::size_t{0}),
                argument.is_null() ? 0 : argument.value("column", std::size_t{0})};
            const auto oldSize = state[index].dump().size();
            combine(state[index], aggregate, values[index], location);
            const auto newSize = state[index].dump().size();
            if (newSize > oldSize && newSize - oldSize > maximumPayload - groupPayloadBytes) fail("Aggregate state payload budget exceeded");
            groupPayloadBytes = groupPayloadBytes - oldSize + newSize;
            if (!external) totalPayloadBytes = totalPayloadBytes - oldSize + newSize;
        }
    }
    if (active) emit();
    return rows;
}
std::unique_ptr<RowStream> Database::scanRowStream(const sql::LogicalPlan& plan) {
    const catalog::StoredTable* table = nullptr;
    for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Plan references missing table");
    return std::make_unique<ScanRowStream>(heap_, table->id, rowSchema(table->definition));
}

std::unique_ptr<RowStream> Database::openRowStream(const sql::LogicalPlan& plan) {
    checkCancelled();
    if (plan.kind == "SeqScan") return scanRowStream(plan);
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
        if (plan.children.size() != 1) fail("Filter requires one child");
        auto child = openRowStream(plan.children.front());
        const auto predicate = plan.predicate;
        return std::make_unique<FilterRowStream>(std::move(child), [this, predicate](const json& row) {
            return accepted(evaluate(predicate, row));
        });
    }
    if (plan.kind == "Project") {
        if (plan.children.size() != 1) fail("Project requires one child");
        auto child = openRowStream(plan.children.front());
        const auto projections = plan.projections;
        const auto output = plan.output;
        return std::make_unique<ProjectRowStream>(std::move(child), [this, projections, output](const json& row) {
            json projected = json::array();
            if (!projections.empty()) {
                for (const auto& expression : projections) projected.push_back(evaluate(expression, row));
            } else {
                for (const auto& column : output) {
                    if (column.columnId >= row.size()) fail("Projection outside row");
                    projected.push_back(cell(row.at(column.columnId)));
                }
            }
            return projected;
        });
    }
    if (plan.kind == "Limit") {
        if (plan.children.size() != 1) fail("Limit requires one child");
        auto child = openRowStream(plan.children.front());
        return std::make_unique<LimitRowStream>(std::move(child), plan.offset, plan.limit);
    }
    if (plan.kind == "Sort" || plan.kind == "Aggregate" || plan.kind == "Distinct") {
        auto result = runNode(plan);
        return std::make_unique<MaterializedRowStream>(result.at("rows"));
    }
    throw MiniSqlError(ErrorCode::InvalidArgument, "RowStream does not support plan kind " + plan.kind);
}

nlohmann::json Database::runNode(const sql::LogicalPlan& plan) {
    checkCancelled();
    if (plan.kind == "Sort") {
        if (plan.children.size() != 1) fail("Sort requires one child");
        auto result = run(plan.children.front());
        auto& rows = result["rows"];
        const auto compareRows = [&](const json& left, const json& right) {
            for (const auto& sort : plan.sortKeys) {
                const auto index = sort.at("index").get<std::size_t>();
                const auto& a = left.at(index);
                const auto& b = right.at(index);
                if (a == b) continue;
                if (a.is_null() || b.is_null()) return a.is_null() ? sort.at("nullsFirst").get<bool>() : !sort.at("nullsFirst").get<bool>();
                if (decimalType(plan.children.front().output.at(index).type)) {
                    const auto& type = plan.children.front().output.at(index).type;
                    const auto order = decimalValue(a, type).compare(decimalValue(b, type));
                    if (order == 0) continue;
                    return sort.at("descending").get<bool>() ? order > 0 : order < 0;
                }
                return sort.at("descending").get<bool>() ? a > b : a < b;
            }
            return false;
        };
        if (rows.size() > sortMemoryRows_) {
            const auto operationId = sessionId_ + "-q" + std::to_string(currentQueryId_) + "-s" + std::to_string(++sortSequence_);
            externalSort(rows, compareRows, sortMemoryRows_, sortTempDirectory_, operationId, [this] { checkCancelled(); });
        }
        else std::stable_sort(rows.begin(), rows.end(), compareRows);
        for (auto& row : rows) while (row.size() > plan.output.size()) row.erase(row.end() - 1);
        result["columns"] = json::array();
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        result["kind"] = "Sort";
        result["resourceUsage"] = {{"kind", "Sort"}, {"rows", rows.size()},
            {"external", rows.size() > sortMemoryRows_}, {"memoryRows", sortMemoryRows_}};
        return result;
    }
    if (plan.kind == "Limit") {
        if (plan.children.size() != 1) fail("Limit requires one child");
        if (plan.limit && *plan.limit == 0) {
            json columns = json::array();
            for (const auto& column : plan.output) columns.push_back(column.name);
            return {{"kind", "Limit"}, {"columns", columns}, {"rows", json::array()}, {"affectedRows", 0}};
        }
        try {
            auto stream = openRowStream(plan.children.front());
            json rows = json::array();
            json row;
            for (std::uint64_t skipped = 0; skipped < plan.offset; ++skipped) {
                if (!stream->next(row)) break;
            }
            std::uint64_t emitted = 0;
            while (!plan.limit || emitted < *plan.limit) {
                if (!stream->next(row)) break;
                rows.push_back(std::move(row));
                ++emitted;
            }
            stream->close();
            json columns = json::array();
            for (const auto& column : plan.output) columns.push_back(column.name);
            return {{"kind", "Limit"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
                {"affectedRows", 0}, {"resourceUsage", {{"kind", "LimitRowStream"}, {"rows", emitted}}}};
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::InvalidArgument) throw;
        }
        auto result = run(plan.children.front());
        const auto size = result.at("rows").size();
        const auto begin = std::min<std::uint64_t>(plan.offset, size);
        const auto count = std::min<std::uint64_t>(plan.limit.value_or(size), size - begin);
        json rows = json::array();
        for (std::uint64_t i = 0; i < count; ++i) rows.push_back(std::move(result["rows"][begin + i]));
        result["rows"] = std::move(rows);
        result["kind"] = "Limit";
        result["resourceUsage"] = {{"kind", "Limit"}, {"rows", rows.size()}};
        return result;
    }
    if (plan.kind == "Distinct") {
        if (plan.children.size() != 1 || plan.children.front().kind != "Project") fail("Distinct requires a projection child");
        auto result = run(plan.children.front());
        std::set<json> seen;
        json unique = json::array();
        for (auto& row : result.at("rows")) if (seen.insert(row).second) unique.push_back(std::move(row));
        result["rows"] = std::move(unique);
        result["kind"] = "Distinct";
        return result;
    }
    json result = {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}};
    if (plan.kind == "Aggregate") {
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        result["rows"] = aggregateRows(plan);
        result["resourceUsage"] = {{"kind", "Aggregate"}, {"rows", result.at("rows").size()},
            {"groups", result.at("rows").size()}, {"external", false}};
        return result;
    }
    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
        if (plan.children.size() != 1) fail("Filter requires one child");
        auto input = run(plan.children.front());
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        for (auto& row : input["rows"]) if (accepted(evaluate(plan.predicate, row))) result["rows"].push_back(std::move(row));
        result["resourceUsage"] = {{"kind", "Filter"}, {"rows", result.at("rows").size()}};
        return result;
    }
    if (plan.kind == "Project" && plan.children.size() == 1 &&
        (plan.children.front().kind == "Aggregate" || (plan.children.front().kind == "Filter" &&
         plan.children.front().children.size() == 1 && plan.children.front().children.front().kind == "Aggregate"))) {
        const auto input = run(plan.children.front());
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        for (const auto& row : input.at("rows")) {
            json projected = json::array();
            for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
            result["rows"].push_back(std::move(projected));
        }
        json projectUsage = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
        if (input.contains("resourceUsage")) projectUsage["child"] = input.at("resourceUsage");
        result["resourceUsage"] = std::move(projectUsage);
        return result;
    }
    if (plan.kind == "Project" && plan.children.size() == 1 && plan.children.front().kind == "Limit" &&
        plan.children.front().limit && *plan.children.front().limit == 0) {
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        return result;
    }
    // X09 3.3: 外层 Select 投影于一个“成形的”子计划（派生表）之上 —— 先物化内层
    // 关系，再对外层投影求值。普通 Select（Filter 下接裸 Scan）不受影响。
    if (plan.kind == "Project" && plan.children.size() == 1) {
        std::function<bool(const sql::LogicalPlan&)> subplanRoot;
        subplanRoot = [&subplanRoot](const sql::LogicalPlan& node) -> bool {
            if (node.kind == "Project" || node.kind == "Aggregate" || node.kind == "Distinct" ||
                node.kind == "Sort" || node.kind == "Limit") return true;
            if (node.kind == "Filter") return !node.children.empty() && subplanRoot(node.children.front());
            return false;
        };
        if (subplanRoot(plan.children.front())) {
            for (const auto& column : plan.output) result["columns"].push_back(column.name);
            const auto sub = run(plan.children.front());
            for (const auto& row : sub.at("rows")) {
                json projected = json::array();
                for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
                result["rows"].push_back(std::move(projected));
            }
            json projectUsage = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
            if (sub.contains("resourceUsage")) projectUsage["child"] = sub.at("resourceUsage");
            result["resourceUsage"] = std::move(projectUsage);
            return result;
        }
    }
    if (plan.kind == "CreateIndex") {
        const catalog::StoredTable* stored = nullptr;
        for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) stored = &candidate;
        if (!stored) fail("Index table does not exist");
        const auto* definition = catalog_.view().find(stored->definition.table);
        if (!definition) fail("Index table definition missing");
        std::vector<std::size_t> columns;
        for (const auto& name : plan.indexColumns) columns.push_back(catalog::resolveColumnIndex(*definition, name));
        if (plan.uniqueIndex) {
            std::set<json> keys;
            heap_.scan(stored->id, rowSchema(stored->definition), [&](storage::RowRef, const storage::Row& row) {
                json key = json::array();
                bool hasNull = false;
                for (const auto column : columns) { key.push_back(cell(row[column]));hasNull = hasNull || key.back().is_null(); }
                if (hasNull) return;
                if (!keys.insert(key).second) fail("UNIQUE index contains duplicate keys");
            });
        }
        sql::Statement indexDefinition;
        indexDefinition.kind = "CreateIndex";indexDefinition.indexName = plan.indexName;
        indexDefinition.uniqueIndex = plan.uniqueIndex;indexDefinition.table = plan.table;
        indexDefinition.indexColumns = plan.indexColumns;
        catalog_.createIndex(indexDefinition);
        rebuildIndexes(stored->id);
        result["kind"] = "CreateIndex";
        return result;
    }
    if (plan.kind == "DropIndex") {
        sql::Statement definition;
        definition.kind = "DropIndex";definition.indexName = plan.indexName;definition.table = plan.table;
        catalog_.dropIndex(definition);
        for (const auto& index : indexes_)
            if (key(index->name) == key(plan.indexName) && (plan.table.empty() || key(index->table) == key(plan.table)))
                clearIndexPages(indexOwnerId(index->table, index->name));
        indexes_.erase(std::remove_if(indexes_.begin(), indexes_.end(), [&](const auto& index) {
            return key(index->name) == key(plan.indexName) && (plan.table.empty() || key(index->table) == key(plan.table));
        }), indexes_.end());
        result["kind"] = "DropIndex";
        return result;
    }
    if (plan.kind == "CreateTable") {
        sql::Statement definition;
        definition.kind = "CreateTable";
        definition.table = plan.table;
        definition.keys = plan.keys;
        definition.foreignKeys = plan.foreignKeys;
        definition.constraintNames = plan.constraintNames;
        for (const auto& check : plan.checkDefinitions) definition.checks.push_back(sql::deserializeExpression(check));
        for (const auto& column : plan.output) definition.columns.push_back({column.name, column.type, column.nullable, column.defaultValue, column.primaryKey, column.unique, column.references});
        catalog_.create(definition);
        return result;
    }
    const catalog::StoredTable* table = nullptr;
    for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(plan.table)) table = &candidate;
    if (!table) fail("Plan references missing table");
    const auto schema = rowSchema(table->definition);
    if (plan.kind == "IndexScan") {
        const RuntimeIndex* index = nullptr;
        for (const auto& candidate : indexes_) if (key(candidate->table) == key(plan.table) && key(candidate->name) == key(plan.indexName)) index = candidate.get();
        if (!index) fail("IndexScan references missing runtime index");
        std::vector<storage::RowRef> refs;
        if (!plan.indexRangeOperator.empty()) {
            storage::IndexKey key;
            for (const auto& value : plan.indexValues)
                key.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
            key.values.push_back(indexValue(plan.indexRangeValue.at("value"), plan.indexRangeValue.at("type").get<std::string>()));
            const auto& op = plan.indexRangeOperator;
            if (op == ">") refs = index->range(key, false, std::nullopt, true);
            else if (op == ">=") refs = index->range(key, true, std::nullopt, true);
            else if (op == "<") refs = index->range(std::nullopt, true, key, false);
            else refs = index->range(std::nullopt, true, key, true);
        } else if (!plan.indexValues.empty()) {
            storage::IndexKey key;
            for (const auto& value : plan.indexValues)
                key.values.push_back(indexValue(value.at("value"), value.at("type").get<std::string>()));
            refs = index->search(key);
        } else {
        const auto& predicate = plan.predicate;
        if (!predicate.is_object()) fail("IndexScan requires a predicate");
        const auto op = predicate.value("operator", "");
        if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=") fail("IndexScan requires an indexed comparison");
        const auto& right = predicate.at("right");
        if (!right.is_object() || right.value("kind", "") != "Literal") fail("IndexScan requires a literal key");
        const auto key = storage::IndexKey{{indexValue(right.at("value"), right.at("type").get<std::string>())}};
        if (op == "=") refs = index->search(key);
        else if (op == ">") refs = index->range(key, false, std::nullopt, true);
        else if (op == ">=") refs = index->range(key, true, std::nullopt, true);
        else if (op == "<") refs = index->range(std::nullopt, true, key, false);
        else refs = index->range(std::nullopt, true, key, true);
        }
        for (const auto ref : refs) result["rows"].push_back(rowJson(heap_.read(table->id, schema, ref)));
        for (const auto& column : plan.output) result["columns"].push_back(column.name);
        return result;
    }

    const auto checkConstraints = plan.checks;
    const auto checkRows = [&](const storage::Row& row) {
        for (std::size_t i = 0; i < checkConstraints.size(); ++i) {
            const auto& check = checkConstraints[i];
            const auto value = evaluate(check, row);
            if (value.is_null() || (value.is_boolean() && value.get<bool>())) continue;
            throw MiniSqlError(ErrorCode::Execution, "CHECK constraint failed" + sql::constraintSuffix(table->definition.constraintNames, "check", i));
        }
    };
    const auto references = sql::allForeignKeys(table->definition);
    const auto isSelfReference = [&](const sql::ForeignKey& reference) { return key(reference.table) == key(table->definition.table); };
    const bool hasSelfReferences = std::any_of(references.begin(), references.end(), isSelfReference);
    const auto checkSelfReferences = [&](const std::vector<storage::Row>& finalRows) {
        for (std::size_t r = 0; r < references.size(); ++r) {
            const auto& reference = references[r];
            if (!isSelfReference(reference)) continue;
            std::vector<std::size_t> parentIndices, childIndices;
            const auto* definition = catalog_.view().find(table->definition.table);
            for (const auto& name : reference.columns) childIndices.push_back(catalog::resolveColumnIndex(*definition, name));
            for (const auto& name : reference.referencedColumns) parentIndices.push_back(catalog::resolveColumnIndex(*definition, name));
            auto tuple = [&](const storage::Row& row, const std::vector<std::size_t>& indices) {
                auto value = json::array();
                for (const auto index : indices) value.push_back(cell(row[index]));
                return value;
            };
            std::set<json> parentKeys;
            for (const auto& row : finalRows) parentKeys.insert(tuple(row, parentIndices));
            for (const auto& row : finalRows) {
                const auto value = tuple(row, childIndices);
                if (std::any_of(value.begin(), value.end(), [](const json& item) { return item.is_null(); })) continue;
                if (!parentKeys.contains(value)) throw MiniSqlError(ErrorCode::Execution, "Self-referencing FOREIGN KEY constraint failed" + sql::foreignKeySuffix(table->definition, r));
            }
        }
    };
    const auto checkForeignKeys = [&](const storage::Row& row) {
        for (std::size_t r = 0; r < references.size(); ++r) {
            const auto& reference = references[r];
            if (isSelfReference(reference)) continue;
            auto values = json::array();bool hasNull = false;
            for (const auto& column : reference.columns) {
                const auto index = catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), column);
                values.push_back(cell(row[index]));hasNull = hasNull || values.back().is_null();
            }
            if (hasNull) continue;
            const catalog::StoredTable* parent = nullptr;
            for (const auto& candidate : catalog_.tables()) if (key(candidate.definition.table) == key(reference.table)) parent = &candidate;
            if (!parent) fail("Referenced table does not exist");
            const auto* parentDefinition = catalog_.view().find(parent->definition.table);
            std::vector<std::size_t> parentIndices;
            for (const auto& column : reference.referencedColumns) parentIndices.push_back(catalog::resolveColumnIndex(*parentDefinition, column));
            const auto parentSchema = rowSchema(parent->definition);
            bool found = false;
            heap_.scan(parent->id, parentSchema, [&](storage::RowRef, const storage::Row& parentRow) {
                if (found) return;
                auto tuple = json::array();
                for (const auto index : parentIndices) tuple.push_back(cell(parentRow[index]));
                found = tuple == values;
            });
            if (!found) throw MiniSqlError(ErrorCode::Execution, "FOREIGN KEY constraint failed" + sql::foreignKeySuffix(table->definition, r));
        }
    };
    std::vector<std::vector<std::size_t>> uniqueColumns;
    const auto restrictParent = [&](storage::RowRef oldRef, const storage::Row& oldRow, const storage::Row* replacement) {
        for (const auto& child : catalog_.tables()) {
            const auto childReferences = sql::allForeignKeys(child.definition);
            for (std::size_t r = 0; r < childReferences.size(); ++r) {
                const auto& reference = childReferences[r];
                if (key(reference.table) != key(table->definition.table)) continue;
                auto oldValues = json::array(), newValues = json::array();bool hasNull = false;
                for (const auto& column : reference.referencedColumns) {
                    const auto index = catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), column);
                    oldValues.push_back(cell(oldRow[index]));hasNull = hasNull || oldValues.back().is_null();
                    if (replacement) newValues.push_back(cell((*replacement)[index]));
                }
                if (hasNull || (replacement && oldValues == newValues)) continue;
                std::vector<std::size_t> childIndices;
                for (const auto& column : reference.columns)
                    childIndices.push_back(catalog::resolveColumnIndex(*catalog_.view().find(child.definition.table), column));
                heap_.scan(child.id, rowSchema(child.definition), [&](storage::RowRef childRef, const storage::Row& childRow) {
                    // 当前行的自引用由候选最终状态检查；其他行仍执行 RESTRICT。
                    if (child.id == table->id && childRef.page.id == oldRef.page.id && childRef.page.generation == oldRef.page.generation &&
                        childRef.slot.slot == oldRef.slot.slot && childRef.slot.generation == oldRef.slot.generation) return;
                    auto tuple = json::array();
                    for (const auto index : childIndices) tuple.push_back(cell(childRow[index]));
                    if (tuple == oldValues) throw MiniSqlError(ErrorCode::Execution, "FOREIGN KEY RESTRICT: parent key is referenced" + sql::foreignKeySuffix(child.definition, r));
                });
            }
        }
    };
    std::vector<bool> primaryKeys;
    std::vector<std::string> uniqueNames;
    for (std::size_t i = 0; i < schema.size(); ++i)
        if (table->definition.columns[i].primaryKey || table->definition.columns[i].unique) {
            uniqueColumns.push_back({i});primaryKeys.push_back(table->definition.columns[i].primaryKey);
            uniqueNames.push_back(sql::constraintSuffix(table->definition.constraintNames, table->definition.columns[i].primaryKey ? "primaryKey" : "unique", i));
        }
    for (std::size_t i = 0; i < table->definition.keys.size(); ++i) {
        const auto& constraint = table->definition.keys[i];
        std::vector<std::size_t> indices;
        for (const auto& name : constraint.columns)
            indices.push_back(catalog::resolveColumnIndex(*catalog_.view().find(table->definition.table), name));
        uniqueColumns.push_back(std::move(indices));primaryKeys.push_back(constraint.primary);
        uniqueNames.push_back(sql::constraintSuffix(table->definition.constraintNames, "key", i));
    }
    std::vector<std::set<json>> uniqueValues(uniqueColumns.size());
    const auto checkUnique = [&](const storage::Row& row) {
        for (std::size_t i = 0; i < uniqueColumns.size(); ++i) {
            auto value = json::array();bool hasNull = false;
            for (const auto index : uniqueColumns[i]) {
                value.push_back(cell(row[index]));hasNull = hasNull || value.back().is_null();
            }
            if (hasNull) {
                if (primaryKeys[i]) throw MiniSqlError(ErrorCode::Execution, "PRIMARY KEY cannot be NULL" + uniqueNames[i]);
                continue;
            }
            if (!uniqueValues[i].insert(value).second)
                throw MiniSqlError(ErrorCode::Execution, "UNIQUE constraint failed on key " + std::to_string(i + 1) + uniqueNames[i]);
        }
    };
    if (plan.kind == "Insert") {
        std::vector<storage::Row> candidates;
        if (!uniqueColumns.empty())
            heap_.scan(table->id, schema, [&](storage::RowRef, const storage::Row& existing) { checkUnique(existing); });
        const auto prepareRow = [&](const json& values, const json& expressions) {
            storage::Row row;
            if (values.size() != schema.size()) fail("Insert plan schema mismatch");
            if (!expressions.empty() && expressions.size() != schema.size()) fail("Insert expression schema mismatch");
            for (std::size_t i = 0; i < schema.size(); ++i) {
                const auto value = expressions.empty() ? values[i] : evaluate(expressions[i], storage::Row{});
                if (value.is_null()) {
                    if (!table->definition.columns[i].nullable) throw MiniSqlError(ErrorCode::Execution, "NOT NULL constraint failed" + catalog::notNullConstraintSuffix(*catalog_.view().find(table->definition.table), i));
                    row.emplace_back(std::monostate{});
                } else if (schema[i] == storage::ColumnType::Int) row.emplace_back(value.get<std::int32_t>());
                else if (schema[i] == storage::ColumnType::Bigint) row.emplace_back(value.get<std::int64_t>());
                else if (schema[i] == storage::ColumnType::Bool) row.emplace_back(value.get<bool>());
                else if (schema[i] == storage::ColumnType::Float) {
                    const auto& expression = expressions.empty() ? json::object() : expressions[i];
                    row.emplace_back(storedFloat(value, {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})}));
                }
                else if (schema[i] == storage::ColumnType::Decimal) {
                    const auto& expression = expressions.empty() ? json::object() : expressions[i];
                    row.emplace_back(storedDecimal(value, schema[i], {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})}));
                } else {
                    const auto text=value.get<std::string>();
                    if (schema[i] == storage::ColumnType::BoundedVarchar) {
                        const auto& expression=expressions.empty() ? json::object() : expressions[i];
                        validateVarchar(text,schema[i].maxLength,{expression.value("line",std::size_t{0}),expression.value("column",std::size_t{0})});
                    }
                    row.emplace_back(text);
                }
            }
            (void)storage::encodeRow(row, schema);
            checkUnique(row);
            checkRows(row);
            checkForeignKeys(row);
            candidates.push_back(std::move(row));
        };
        if (plan.insertRows.empty()) prepareRow(plan.values, plan.insertExpressions);
        else for (const auto& row : plan.insertRows) prepareRow(row.at("values"), row.at("expressions"));
        if (hasSelfReferences) {
            std::vector<storage::Row> finalRows;
            heap_.scan(table->id, schema, [&](storage::RowRef, const storage::Row& existing) { finalRows.push_back(existing); });
            finalRows.insert(finalRows.end(), candidates.begin(), candidates.end());
            checkSelfReferences(finalRows);
        }
        for (const auto& row : candidates) validateUniqueIndexes(table->id, row);
        for (const auto& row : candidates) heap_.insert(table->id, schema, row);
        if (!indexes_.empty()) rebuildIndexes(table->id);
        heap_.flush();
        result["affectedRows"] = candidates.size();
        return result;
    }
    if (plan.kind != "Project" && plan.kind != "Delete" && plan.kind != "Update") fail("Unsupported root plan");
    if (plan.children.size() != 1) fail("Root plan requires one child");
    if (plan.kind == "Project") {
        try {
            auto stream = openRowStream(plan);
            json rows = json::array();
            json row;
            while (stream->next(row)) rows.push_back(std::move(row));
            stream->close();
            json columns = json::array();
            for (const auto& column : plan.output) columns.push_back(column.name);
            return {{"kind", "Project"}, {"columns", std::move(columns)}, {"rows", std::move(rows)},
                {"affectedRows", 0}, {"resourceUsage", stream->resourceUsage()}};
        } catch (const MiniSqlError& error) {
            if (error.code() != ErrorCode::InvalidArgument) throw;
        }
    }
    const auto* input = &plan.children.front();
    const json* predicate = nullptr;
    if (input->kind == "Filter" || input->kind == "SemiJoin" || input->kind == "AntiJoin" || input->kind == "Apply") {
        predicate = &input->predicate;
        if (input->children.size() != 1) fail("Filter requires one child");
        input = &input->children.front();
    }
    const bool joined = (input->kind == "NestedLoopJoin" || input->kind == "HashJoin" || input->kind == "LeftJoin" || input->kind == "RightJoin" || input->kind == "FullJoin") && plan.kind == "Project";
    if (!joined && ((input->kind != "SeqScan" && input->kind != "IndexScan") || key(input->table) != key(plan.table))) fail("Unsupported scan plan");
    for (const auto& column : plan.output) result["columns"].push_back(column.name);
    if (input->kind == "IndexScan" && plan.kind == "Project") {
        const auto indexed = run(*input);
        for (const auto& row : indexed.at("rows")) {
            if (predicate && !accepted(evaluate(*predicate, row))) continue;
            json projected = json::array();
            if (!plan.projections.empty()) for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
            else for (const auto& column : plan.output) projected.push_back(row.at(column.columnId));
            result["rows"].push_back(std::move(projected));
        }
        return result;
    }
    std::vector<storage::RowRef> deletion;
    std::vector<std::pair<storage::RowRef, storage::Row>> updates;
    const bool inspectSelfReferences = hasSelfReferences && (plan.kind == "Update" || plan.kind == "Delete");
    std::vector<storage::Row> finalRows;
    const auto consume = [&](storage::RowRef ref, const storage::Row& row) {
        checkCancelled();
        if (predicate && !accepted(evaluate(*predicate, row))) {
            if (plan.kind == "Update") checkUnique(row);
            if (inspectSelfReferences) finalRows.push_back(row);
            return;
        }
        if (plan.kind == "Delete") { restrictParent(ref, row, nullptr); deletion.push_back(ref); return; }
        if (plan.kind == "Update") {
            if (plan.columnMapping.size() != plan.projections.size()) fail("UPDATE assignment mapping mismatch");
            auto replacement = row;
            for (std::size_t i = 0; i < plan.columnMapping.size(); ++i) {
                const auto index = plan.columnMapping[i];
                if (index >= row.size()) fail("UPDATE column outside row");
                const auto value = evaluate(plan.projections[i], row);
                if (value.is_null()) {
                    if (!table->definition.columns[index].nullable) throw MiniSqlError(ErrorCode::Execution, "NOT NULL constraint failed" + catalog::notNullConstraintSuffix(*catalog_.view().find(table->definition.table), index));
                    replacement[index] = std::monostate{};
                } else if (schema[index] == storage::ColumnType::Int) replacement[index] = value.get<std::int32_t>();
                else if (schema[index] == storage::ColumnType::Bigint) replacement[index] = value.get<std::int64_t>();
                else if (schema[index] == storage::ColumnType::Bool) replacement[index] = value.get<bool>();
                else if (schema[index] == storage::ColumnType::Float) {
                    const auto& expression = plan.projections[i];
                    replacement[index] = storedFloat(value, {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
                }
                else if (schema[index] == storage::ColumnType::Decimal) {
                    const auto& expression = plan.projections[i];
                    replacement[index] = storedDecimal(value, schema[index], {expression.value("line", std::size_t{0}), expression.value("column", std::size_t{0})});
                } else {
                    const auto text=value.get<std::string>();
                    if (schema[index] == storage::ColumnType::BoundedVarchar) {
                        const auto& expression=plan.projections[i];
                        validateVarchar(text,schema[index].maxLength,{expression.value("line",std::size_t{0}),expression.value("column",std::size_t{0})});
                    }
                    replacement[index]=text;
                }
            }
            (void)storage::encodeRow(replacement, schema);
            checkUnique(replacement);
            checkRows(replacement);
            checkForeignKeys(replacement);
            restrictParent(ref, row, &replacement);
            if (inspectSelfReferences) finalRows.push_back(replacement);
            updates.emplace_back(ref, std::move(replacement));
            return;
        }
        json projected = json::array();
        if (!plan.projections.empty()) {
            for (const auto& expression : plan.projections) projected.push_back(evaluate(expression, row));
        } else for (const auto& column : plan.output) {
            if (column.columnId >= row.size()) fail("Projection outside row");
            projected.push_back(cell(row[column.columnId]));
        }
        result["rows"].push_back(std::move(projected));
    };
    if (joined) {
        for (const auto& row : joinRows(*input)) consume({}, row);
    } else heap_.scan(table->id, schema, consume);
    if (inspectSelfReferences) checkSelfReferences(finalRows);
    for (auto ref : deletion) heap_.erase(table->id, ref);
    // 扫描及全部表达式检查完成后再写入，避免除零或行长错误造成前半批修改。
    for (const auto& [ref, replacement] : updates) validateUniqueIndexes(table->id, replacement, ref);
    for (const auto& [ref, replacement] : updates) (void)heap_.replace(table->id, schema, ref, replacement);
    if (!indexes_.empty() && (!deletion.empty() || !updates.empty())) rebuildIndexes(table->id);
    if (plan.kind == "Update") { heap_.flush(); result["affectedRows"] = updates.size(); }
    if (plan.kind == "Delete") { heap_.flush(); result["affectedRows"] = deletion.size(); }
    if (plan.kind == "Project") result["resourceUsage"] = {{"kind", "Project"}, {"rows", result.at("rows").size()}};
    return result;
}
const char* Database::transactionState() const {
    if (unavailable_) return "UNKNOWN";
    return transaction_ == TransactionState::Active ? "ACTIVE" : transaction_ == TransactionState::Aborted ? "ABORTED" : "IDLE";
}
void Database::rollbackBatch() {
    try {
        if (file_->writeBatchActive()) buffer_.rollbackWriteBatch();
        catalog_.reload();
        savepoints_.clear();
        for (const auto& table : catalog_.tables()) rebuildIndexes(table.id);
    } catch (...) {
        unavailable_ = true;
        throw MiniSqlError(ErrorCode::Storage, "Commit state unknown; reopen for recovery");
    }
}
nlohmann::json Database::run(const sql::LogicalPlan& plan) {
    checkCancelled();
    const auto started = std::chrono::steady_clock::now();
    auto result = runNode(plan);
    if (nodeStats_) {
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        nodeStats_->push_back({{"kind", plan.kind}, {"table", plan.table}, {"actualRows", result.value("rows", json::array()).size()}, {"durationMs", elapsed}, {"loops", 1}});
    }
    return result;
}
nlohmann::json Database::runStatement(const sql::LogicalPlan& plan) {
    correlatedRowsCache_.clear();
    if (plan.kind == "IndexInspect") return indexInspect(plan.table, plan.indexName);
    if (plan.kind == "Checkpoint") {
        if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "CHECKPOINT requires an idle transaction");
        buffer_.flushAll();
        file_->checkpoint({catalogVersion_, indexVersion_});
        ++checkpointCount_;
        pendingAutoCheckpointWrites_ = 0;
        pendingAutoCheckpointWalBytes_ = 0;
        lastCheckpointAt_ = std::chrono::steady_clock::now();
        lastCheckpointAtMs_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "committed"}};
    }
    if (plan.kind == "Rollback") {
        if (transaction_ == TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "No active transaction");
        rollbackBatch();transaction_ = TransactionState::Idle;
        transactionWriteStatements_ = 0;
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "rolledBack"}};
    }
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
    if (plan.kind == "Savepoint") {
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "SAVEPOINT requires an active transaction");
        if (plan.savepointName.empty()) throw MiniSqlError(ErrorCode::Transaction, "Savepoint name is required");
        savepoints_[key(plan.savepointName)] = SavepointState{file_->savepoint(), catalog_.snapshot()};
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
    }
    if (plan.kind == "ReleaseSavepoint") {
        if (transaction_ != TransactionState::Active || !savepoints_.erase(key(plan.savepointName)))
            throw MiniSqlError(ErrorCode::Transaction, "Savepoint does not exist: " + plan.savepointName);
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
    }
    if (plan.kind == "RollbackTo") {
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "ROLLBACK TO requires an active transaction");
        const auto found = savepoints_.find(key(plan.savepointName));
        if (found == savepoints_.end()) throw MiniSqlError(ErrorCode::Transaction, "Savepoint does not exist: " + plan.savepointName);
        file_->restoreSavepoint(found->second.file);
        catalog_.restore(found->second.catalog);
        for (const auto& table : catalog_.tables()) rebuildIndexes(table.id);
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
    }
    if (plan.kind == "Begin") {
        if (transaction_ != TransactionState::Idle) throw MiniSqlError(ErrorCode::Transaction, "Nested transactions are not supported");
        buffer_.beginWriteBatch();transaction_ = TransactionState::Active;transactionWriteStatements_ = 0;savepoints_.clear();
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "pending"}};
    }
    if (plan.kind == "Commit") {
        if (transaction_ != TransactionState::Active) throw MiniSqlError(ErrorCode::Transaction, "No active transaction");
        const auto committedWriteStatements = transactionWriteStatements_;
        const auto committedDirtyPages = file_->stagedPageCount();
        buffer_.commitWriteBatch();transaction_ = TransactionState::Idle;transactionWriteStatements_ = 0;savepoints_.clear();
        evaluateAutoCheckpoint(committedWriteStatements, committedDirtyPages);
        return {{"kind", plan.kind}, {"columns", json::array()}, {"rows", json::array()}, {"affectedRows", 0}, {"commitState", "committed"}};
    }
    const bool writes = plan.kind == "CreateTable" || plan.kind == "CreateIndex" || plan.kind == "DropIndex" || plan.kind == "Insert" ||
                        plan.kind == "Update" || plan.kind == "Delete";
    if (transaction_ == TransactionState::Active) {
        auto result = run(plan);
        if (writes) ++transactionWriteStatements_;
        return result;
    }
    if (!writes) return run(plan);
    buffer_.beginWriteBatch();
    try {
        auto result = run(plan);
        const auto committedDirtyPages = file_->stagedPageCount();
        buffer_.commitWriteBatch();
        evaluateAutoCheckpoint(1, committedDirtyPages);
        return result;
    } catch (...) {
        rollbackBatch();
        throw;
    }
}
nlohmann::json Database::diagnostics(const std::string& source) const {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    json items = json::array();
    const auto stageFor = [](ErrorCode code) {
        if (code == ErrorCode::Lexical) return "lexer";
        if (code == ErrorCode::Syntax) return "parser";
        if (code == ErrorCode::Semantic || code == ErrorCode::Catalog) return "semantic";
        if (code == ErrorCode::NotImplemented) return "planner";
        return "internal";
    };
    std::size_t statementIndex = 0;
    const auto sourceLine = [&](std::size_t line) {
        if (line == 0) return std::string{};
        std::size_t current = 1, begin = 0;
        while (begin <= source.size() && current < line) {
            const auto end = source.find('\n', begin);
            if (end == std::string::npos) return std::string{};
            begin = end + 1;
            ++current;
        }
        const auto end = source.find('\n', begin);
        return source.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    };
    const auto closestName = [](const std::string& target, const std::vector<std::string>& candidates) {
        if (target.empty()) return std::string{};
        const auto lower = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        };
        const auto normalizedTarget = lower(target);
        std::string best;
        std::size_t bestDistance = std::numeric_limits<std::size_t>::max();
        for (const auto& candidate : candidates) {
            const auto normalizedCandidate = lower(candidate);
            std::vector<std::size_t> previous(normalizedCandidate.size() + 1), current(normalizedCandidate.size() + 1);
            for (std::size_t i = 0; i <= normalizedCandidate.size(); ++i) previous[i] = i;
            for (std::size_t i = 1; i <= normalizedTarget.size(); ++i) {
                current[0] = i;
                for (std::size_t j = 1; j <= normalizedCandidate.size(); ++j)
                    current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                        previous[j - 1] + (normalizedTarget[i - 1] == normalizedCandidate[j - 1] ? 0 : 1)});
                std::swap(previous, current);
            }
            const auto distance = previous.back();
            if (distance < bestDistance || (distance == bestDistance && normalizedCandidate < lower(best))) {
                bestDistance = distance;
                best = candidate;
            }
        }
        const auto limit = std::max<std::size_t>(1, std::min<std::size_t>(3, normalizedTarget.size() / 2 + 1));
        return bestDistance <= limit ? best : std::string{};
    };
    const auto append = [&](const MiniSqlError& error) {
        const auto& loc = error.location();
        std::string suggestion = error.suggestion();
        if (suggestion.empty()) {
            const std::string message = error.what();
            if (message.find("Table does not exist") != std::string::npos || message.find("missing table") != std::string::npos)
            {
                const auto separator = message.find(':');
                const auto target = message.substr(separator == std::string::npos ? 0 : separator + 1);
                std::vector<std::string> candidates;
                for (const auto& table : catalog_.tables()) candidates.push_back(table.definition.table);
                const auto matched = closestName(target, candidates);
                suggestion = matched.empty() ? "Check the table name and confirm the table was created." : "Did you mean: " + matched + "?";
            }
            else if (message.find("Column does not exist") != std::string::npos || message.find("Unknown column") != std::string::npos)
            {
                const auto separator = message.find(':');
                const auto target = message.substr(separator == std::string::npos ? 0 : separator + 1);
                std::vector<std::string> candidates;
                for (const auto& table : catalog_.tables()) for (const auto& column : table.definition.columns) candidates.push_back(column.name);
                const auto matched = closestName(target, candidates);
                suggestion = matched.empty() ? "Check the column name and table alias." : "Did you mean: " + matched + "?";
            }
            else if (message.find("Expected FROM") != std::string::npos)
                suggestion = "Add FROM before the table name.";
        }
        items.push_back({{"success", false}, {"stage", stageFor(error.code())},
            {"code", static_cast<int>(error.code())}, {"message", error.what()},
            {"suggestion", std::move(suggestion)}, {"actual", error.actual()},
            {"expected", error.expected()},
            {"line", loc.line}, {"column", loc.column},
            {"endLine", loc.endLine ? loc.endLine : loc.line},
            {"endColumn", loc.endColumn ? loc.endColumn : loc.column},
            {"source", sourceLine(loc.line)},
            {"recoverable", true}, {"statementIndex", statementIndex}});
    };
    // Tokenize in recovery mode so every lexical error is reported, not only
    // the first one. Valid tokens still come back for later statements.
    std::vector<MiniSqlError> lexicalErrors;
    const auto tokens = sql::tokenizeRecoverable(source, lexicalErrors);
    for (const auto& error : lexicalErrors) append(error);
    if (!lexicalErrors.empty() && tokens.size() <= 1) {
        return {{"success", false}, {"diagnostics", items}, {"count", items.size()}};
    }

    catalog::Catalog snapshot = catalog_.view();
    std::vector<sql::Token> statement;
    const auto process = [&]() {
        if (statement.empty()) return;
        std::vector<MiniSqlError> syntaxErrors;
        const auto ast = sql::parseRecoverable(statement, syntaxErrors);
        for (const auto& error : syntaxErrors) append(error);
        for (const auto& item : ast) {
            if (item.invalid) continue; // offending statement; already reported above
            try {
                const auto nextSnapshot = catalog::compileSnapshot({item}, snapshot);
                (void)sql::compilePlans({item}, snapshot);
                snapshot = nextSnapshot;
            } catch (const MiniSqlError& error) { append(error); continue; }
            items.push_back({{"success", true}, {"stage", "passed"}, {"kind", item.kind},
                {"line", item.location.line}, {"column", item.location.column},
                {"endLine", item.location.endLine ? item.location.endLine : item.location.line},
                {"endColumn", item.location.endColumn ? item.location.endColumn : item.location.column},
                {"source", sourceLine(item.location.line)},
                {"statementIndex", statementIndex}});
        }
        ++statementIndex;
        statement.clear();
    };
    for (const auto& token : tokens) {
        if (token.type == "END") { process(); break; }
        statement.push_back(token);
        if (token.type == "DELIMITER" && token.lexeme == ";") process();
    }
    const bool success = std::all_of(items.begin(), items.end(), [](const json& item) { return item.value("success", false); });
    return {{"success", success}, {"diagnostics", items}, {"count", items.size()}};
}
nlohmann::json Database::runCorrelatedSubquery(const json& expression, const json& row) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    const auto sql = expression.at("subquerySql").get<std::string>();
    const auto& scope = expression.at("outerColumns");
    OuterBinding outer;
    outer.reserve(scope.size());
    for (auto it = scope.begin(); it != scope.end(); ++it)
        outer.emplace(it.key(),
                      std::make_pair(it.value().at("columnId").get<std::size_t>(),
                                     it.value().at("type").get<std::string>()));
    // 缓存按 subquerySql 解析的结构化 AST，执行时以 by-value 参数绑定替换外层列，
    // 避免逐行文本重解析与字面量改写；仍以当前 catalog 编译，保证 schema 变更生效。
    auto& ast = correlatedAstCache_[sql];
    if (ast.empty()) ast = sql::parse(sql::tokenize(sql + ";"));
    if (ast.size() != 1 || ast.front().kind != "Select")
        throw MiniSqlError(ErrorCode::Semantic, "Correlated subquery must be SELECT");

    // 相关子查询「保守执行优化」：结果仅取决于被引用的外层列绑定值。以
    // (subquerySql|scope) 标识相关形状、以绑定值分组，对每个不同参数物化子查询一次
    // （集合语义半连接），结果在单条语句生命周期内复用，避免对重复参数逐行重执行。
    const std::string prepKey = sql + "\x1f" + scope.dump();
    auto& referenced = correlatedColumnsCache_[prepKey];
    if (referenced.empty()) {
        std::set<std::size_t> ids;
        collectStatementOuterReferences(ast.front(), outer, ids);
        referenced.assign(ids.begin(), ids.end());
    }
    json tuple = json::array();
    for (const auto id : referenced) {
        if (id >= row.size()) fail("Correlated subquery outer column outside row");
        tuple.push_back(row.at(id));
    }
    const std::string fullKey = prepKey + "\x1f" + tuple.dump();
    const auto cached = correlatedRowsCache_.find(fullKey);
    if (cached != correlatedRowsCache_.end()) return cached->second;

    sql::Statement bound = bindOuterStatement(ast.front(), outer, row);
    const auto subplans = sql::compilePlans({std::move(bound)}, catalog_.view());
    const auto result = run(subplans.front());
    auto rows = result.at("rows");
    correlatedRowsCache_.emplace(std::move(fullKey), rows);
    return rows;
}
void Database::materializeSubqueries(std::vector<sql::LogicalPlan>& plans) {
    const auto isCorrelated = [&](const json& expression) {
        if (!expression.contains("outerColumns") || !expression.at("outerColumns").is_object()) return false;
        const auto tokens = sql::tokenize(expression.at("subquerySql").get<std::string>());
        for (std::size_t index = 0; index + 2 < tokens.size(); ++index)
            if (tokens[index].type == "IDENTIFIER" && tokens[index + 1].lexeme == "." && tokens[index + 2].type == "IDENTIFIER" &&
                expression.at("outerColumns").contains(key(tokens[index].lexeme + "." + tokens[index + 2].lexeme))) return true;
        return false;
    };

    struct SubqueryResult { json rows; std::string type; };
    const auto executeSubquery = [&](const std::string& sql) -> SubqueryResult {
        const auto ast = sql::parse(sql::tokenize(sql + ";"));
        if (ast.size() != 1 || ast.front().kind != "Select")
            throw MiniSqlError(ErrorCode::Semantic, "Subquery must be a single SELECT");
        const auto subplans = sql::compilePlans(ast, catalog_.view());
        const auto result = run(subplans.front());
        if (result.at("columns").size() != 1)
            throw MiniSqlError(ErrorCode::Semantic, "IN subquery must return exactly one column");
        return {result.at("rows"), subplans.front().output.front().type};
    };
    const auto literalNode = [](const json& value, const std::string& type, const json& origin) {
        json node = {{"kind", "Literal"}, {"type", value.is_null() ? "null" : type}, {"value", value},
            {"line", origin.value("line", 0)}, {"column", origin.value("column", 0)}, {"nullable", value.is_null()}};
        return node;
    };
    std::function<json(json)> rewrite;
    rewrite = [&](json expression) -> json {
        if (!expression.is_object()) return expression;
        const auto kind = expression.value("kind", "");
        if (kind == "Exists") {
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedExists"; return expression; }
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
            return literalNode(!result.rows.empty(), "bool", expression);
        }
        if (kind == "ScalarSubquery") {
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedScalarSubquery"; return expression; }
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
            if (result.rows.empty()) return literalNode(nullptr, "null", expression);
            if (result.rows.size() != 1 || !result.rows.front().is_array() || result.rows.front().size() != 1)
                throw MiniSqlError(ErrorCode::Execution, "Scalar subquery returned more than one row or column");
            return literalNode(result.rows.front().front(), result.type, expression);
        }
        if (kind == "InSubquery") {
            if (isCorrelated(expression)) { expression["kind"] = "CorrelatedInSubquery"; return expression; }
            auto left = rewrite(expression.at("left"));
            const auto result = executeSubquery(expression.at("subquerySql").get<std::string>());
            json combined;
            bool first = true;
            for (const auto& row : result.rows) {
                if (!row.is_array() || row.size() != 1) throw MiniSqlError(ErrorCode::Semantic, "IN subquery must return one scalar column");
                const auto& value = row.front();
                json comparison = {{"kind", "Binary"}, {"operator", "="}, {"type", "bool"}, {"nullable", true},
                    {"left", left}, {"right", literalNode(value, result.type, expression)},
                    {"line", expression.value("line", 0)}, {"column", expression.value("column", 0)}};
                if (first) { combined = std::move(comparison); first = false; }
                else combined = {{"kind", "Binary"}, {"operator", "OR"}, {"type", "bool"}, {"nullable", true},
                    {"left", std::move(combined)}, {"right", std::move(comparison)},
                    {"line", expression.value("line", 0)}, {"column", expression.value("column", 0)}};
            }
            if (first) return literalNode(false, "bool", expression);
            return combined;
        }
        if (expression.contains("left") && !expression.at("left").is_null()) expression["left"] = rewrite(expression.at("left"));
        if (expression.contains("right") && !expression.at("right").is_null()) expression["right"] = rewrite(expression.at("right"));
        return expression;
    };
    std::function<void(sql::LogicalPlan&)> visit;
    visit = [&](sql::LogicalPlan& plan) {
        plan.predicate = rewrite(plan.predicate);
        for (auto& expression : plan.projections) expression = rewrite(expression);
        for (auto& expression : plan.groupKeys) expression = rewrite(expression);
        for (auto& aggregate : plan.aggregates) if (!aggregate.at("argument").is_null()) aggregate["argument"] = rewrite(aggregate.at("argument"));
        for (auto& expression : plan.insertExpressions) expression = rewrite(expression);
        for (auto& row : plan.insertRows) for (auto& expression : row.at("expressions")) expression = rewrite(expression);
        for (auto& child : plan.children) visit(child);
    };
    for (auto& plan : plans) visit(plan);
}
nlohmann::json Database::execute(const std::string& source, bool optimize) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    json results = json::array();
    currentQueryId_ = ++querySequence_;
    try {
        requireAvailable();
        checkCancelled();
        ActiveDatabaseScope active(this);
        // 按词法语句边界逐条分析，保留已成功语句的提交结果。
        std::vector<sql::Token> statement;
        sql::scanTokens(source, [&](const sql::Token& token) {
            if (token.type == "END") {
                if (!statement.empty()) {
                    statement.push_back(token);
                    (void)sql::parse(statement);
                }
                return;
            }
            statement.push_back(token);
            if (token.lexeme != ";" || token.type != "DELIMITER") return;
            if (key(statement.front().lexeme) == "explain") {
                const auto location = statement.front().location;
                if (transaction_ == TransactionState::Aborted)
                    throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
                statement.erase(statement.begin());
                const bool analyze = !statement.empty() && key(statement.front().lexeme) == "analyze";
                if (analyze) statement.erase(statement.begin());
                const auto target = sql::parse(statement);
                if (target.size() != 1 || target.front().kind == "Begin" || target.front().kind == "Commit" || target.front().kind == "Rollback")
                    throw MiniSqlError(ErrorCode::Syntax, "EXPLAIN requires one data query or data definition statement", location);
                const auto rawPlans = sql::compilePlans(target, catalog_.view());
                if (analyze && target.front().kind != "Select")
                    throw MiniSqlError(ErrorCode::Semantic, "EXPLAIN ANALYZE permits only SELECT", location);
                const auto optimized = optimizer::optimize(rawPlans);
                const auto raw = sql::serializePlans(rawPlans);
                const auto optimizedJson = sql::serializePlans(optimized.plans);
                const auto tableEstimate = [&](const std::string& name) -> std::pair<double, double> {
                    for (const auto& table : catalog_.tables()) if (key(table.definition.table) == key(name)) {
                        double rows = 0;
                        heap_.scan(table.id, rowSchema(table.definition), [&](storage::RowRef, const storage::Row&) { ++rows; });
                        return {rows, static_cast<double>(file_->pagesFor(table.id).size())};
                    }
                    return {0, 0};
                };
                const auto statsDocument = statistics();
                std::map<std::string, const json*> statsByTable;
                for (const auto& table : statsDocument.at("tables")) statsByTable[key(table.at("name").get<std::string>())] = &table;
                const auto columnStat = [&](const std::string& table, std::size_t columnId) -> const json* {
                    const auto found = statsByTable.find(key(table));
                    if (found == statsByTable.end()) return nullptr;
                    for (const auto& column : found->second->at("columns"))
                        if (column.at("columnId").get<std::size_t>() == columnId) return &column;
                    return nullptr;
                };
                std::function<double(const json&, const std::string&)> selectivity;
                selectivity = [&](const json& predicate, const std::string& table) -> double {
                    if (!predicate.is_object()) return 1.0;
                    if (predicate.value("kind", "") == "Literal") {
                        const auto& value = predicate.at("value");
                        if (value.is_null() || value == false) return 0.0;
                        if (value == true) return 1.0;
                        return 0.25;
                    }
                    const auto op = predicate.value("operator", "");
                    if (op == "AND") return selectivity(predicate.at("left"), table) * selectivity(predicate.at("right"), table);
                    if (op == "OR") {
                        const auto left = selectivity(predicate.at("left"), table), right = selectivity(predicate.at("right"), table);
                        return std::min(1.0, left + right - left * right);
                    }
                    if (op == "NOT") return 1.0 - selectivity(predicate.at("left"), table);
                    if ((op == "IS NULL" || op == "IS NOT NULL") && predicate.contains("left")) {
                        const auto& operand = predicate.at("left");
                        if (operand.value("kind", "") == "Identifier") {
                            const auto* stat = columnStat(table, operand.at("columnId").get<std::size_t>());
                            if (stat) {
                                const auto ratio = stat->value("nullRatio", 0.0);
                                return op == "IS NULL" ? ratio : 1.0 - ratio;
                            }
                        }
                    }
                    if (predicate.contains("left") && predicate.contains("right")) {
                        const auto& left = predicate.at("left");
                        const auto& right = predicate.at("right");
                        const json* id = left.value("kind", "") == "Identifier" ? &left : right.value("kind", "") == "Identifier" ? &right : nullptr;
                        const json* literal = left.value("kind", "") == "Literal" ? &left : right.value("kind", "") == "Literal" ? &right : nullptr;
                        if (id && literal && id->contains("columnId")) {
                            const auto* stat = columnStat(table, id->at("columnId").get<std::size_t>());
                            if (stat && op == "=") {
                                const auto distinct = stat->value("distinctCount", std::size_t{0});
                                if (distinct > 0) return 1.0 / static_cast<double>(distinct);
                            }
                        }
                    }
                    if (op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") return 0.33;
                    return 0.25;
                };
                std::function<std::pair<double, double>(const sql::LogicalPlan&)> estimate;
                estimate = [&](const sql::LogicalPlan& plan) -> std::pair<double, double> {
                    if (plan.kind == "SeqScan") {
                        const auto [rows, pages] = tableEstimate(plan.table);
                        return {rows, rows + pages};
                    }
                    if (plan.children.empty()) {
                        if (plan.kind == "Insert") return {static_cast<double>(std::max(plan.values.size(), plan.insertRows.size())), 1.0};
                        return {0.0, 1.0};
                    }
                    auto child = estimate(plan.children.front());
                    if (plan.kind == "Filter" || plan.kind == "SemiJoin" || plan.kind == "AntiJoin" || plan.kind == "Apply") {
                        const auto rows = child.first * selectivity(plan.predicate, plan.table);
                        return {rows, child.second + rows};
                    }
                    if (plan.kind == "Sort") {
                        const auto rows = child.first;
                        return {rows, child.second + rows * std::log2(std::max(1.0, rows))};
                    }
                    if (plan.kind == "Limit") return {plan.limit ? std::min(child.first, static_cast<double>(*plan.limit)) : child.first, child.second};
                    if (plan.kind == "Distinct") return {child.first * 0.5, child.second + child.first * 0.5};
                    if (plan.kind == "Aggregate") return {plan.groupKeys.empty() ? 1.0 : std::min(child.first * 0.1, 1000.0), child.second + child.first};
                    if (plan.kind == "NestedLoopJoin" || plan.kind == "LeftJoin" || plan.kind == "RightJoin" || plan.kind == "FullJoin") {
                        if (plan.children.size() != 2) return {0.0, child.second};
                        const auto right = estimate(plan.children[1]);
                        return {child.first * right.first * 0.1, child.second + right.second + child.first * right.first * 0.1};
                    }
                    return {child.first, child.second + child.first};
                };
                std::vector<const sql::LogicalPlan*> planNodes;
                std::function<void(const sql::LogicalPlan&)> collect;
                collect = [&](const sql::LogicalPlan& plan) {
                    planNodes.push_back(&plan);
                    for (const auto& child : plan.children) collect(child);
                };
                for (const auto& plan : (optimize ? optimized.plans : rawPlans)) collect(plan);
                json rows = json::array();
                for (const auto& node : (optimize ? optimizedJson : raw)) {
                    const auto planIndex = node.at("id").get<std::size_t>();
                    if (planIndex >= planNodes.size()) fail("EXPLAIN plan node index is invalid");
                    const auto estimated = estimate(*planNodes[planIndex]);
                    rows.push_back({node.at("kind"), node.at("detail"), estimated.first, estimated.second, "stats-v1", "table-column-statistics-or-default"});
                }
                json accessCandidates = json::array();
                double bestCost = std::numeric_limits<double>::infinity();
                std::string chosenAccess;
                for (const auto* node : planNodes) {
                    if (node->kind != "SeqScan" && node->kind != "IndexScan") continue;
                    const auto candidate = estimate(*node);
                    accessCandidates.push_back({{"kind", node->kind}, {"table", node->table},
                        {"estimatedRows", candidate.first}, {"estimatedCost", candidate.second}});
                    if (candidate.second < bestCost || (candidate.second == bestCost && (chosenAccess.empty() || node->kind < chosenAccess))) {
                        bestCost = candidate.second;
                        chosenAccess = node->kind;
                    }
                }
                json explanation = {{"kind", "Explain"}, {"columns", {"node", "detail", "estimatedRows", "estimatedCost", "estimateSource", "statsSource"}},
                    {"columnTypes", {"varchar", "varchar", "bigint", "float", "varchar", "varchar"}}, {"rows", rows}, {"affectedRows", 0},
                    {"plan", raw}, {"optimizedPlan", optimizedJson}, {"optimizationRules", optimized.changes},
                    {"estimatedRowsAvailable", true}, {"costModel", "stats-v1"}, {"costModelVersion", 1},
                    {"deterministicTieBreak", "estimated-cost-then-plan-kind"}, {"candidateAccessPaths", accessCandidates},
                    {"chosenAccessPath", chosenAccess.empty() ? nullptr : json(chosenAccess)}, {"executed", false},
                    {"commitState", "notApplicable"}};
                if (analyze) {
                    const auto before = buffer_.stats();
                    const auto ioBefore = file_->ioStats();
                    auto actualPlans = optimize ? optimized.plans : rawPlans;
                    materializeSubqueries(actualPlans);
                    correlatedRowsCache_.clear();
                    std::vector<json> nodeStatistics;
                    nodeStats_ = &nodeStatistics;
                    const auto started = std::chrono::steady_clock::now();
                    json actual;
                    try { actual = run(actualPlans.front()); }
                    catch (...) { nodeStats_ = nullptr; throw; }
                    nodeStats_ = nullptr;
                    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                    const auto after = buffer_.stats();
                    const auto ioAfter = file_->ioStats();
                    explanation["kind"] = "ExplainAnalyze";
                    explanation["executed"] = true;
                    explanation["executionStats"] = {{"scope", "query"}, {"nodeStatisticsAvailable", true}, {"nodeStatistics", nodeStatistics},
                        {"actualRows", actual.at("rows").size()}, {"durationMs", elapsed}, {"loops", 1},
                        {"hits", after.hits - before.hits}, {"misses", after.misses - before.misses},
                        {"diskReads", ioAfter.reads - ioBefore.reads}, {"diskWrites", ioAfter.writes - ioBefore.writes},
                        {"diskScope", "database-file-pages-including-header"},
                        {"stagedPageReads", after.stagedPageReads - before.stagedPageReads},
                        {"stagedPageWrites", after.stagedPageWrites - before.stagedPageWrites},
                        {"ioErrors", ioAfter.errors - ioBefore.errors}};
                }
                results.push_back(std::move(explanation));
                statement.clear();
                return;
            }
            auto ast = sql::parse(statement);
            if (transaction_ == TransactionState::Aborted && ast.front().kind != "Rollback")
                throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
            auto plans = sql::compilePlans(ast, catalog_.view());
            if (optimize) plans = optimizer::optimize(plans).plans;
            materializeSubqueries(plans);
            for (const auto& plan : plans) {
                auto result = runStatement(plan);
                if (plan.kind == "Commit" || plan.kind == "Rollback")
                    for (auto& previous : results) if (previous.value("commitState", "") == "pending")
                        previous["commitState"] = plan.kind == "Commit" ? "committed" : "rolledBack";
                if (!result.contains("commitState")) result["commitState"] = transaction_ == TransactionState::Active ? "pending" : "committed";
                result["columnTypes"] = json::array();
                for (const auto& column : plan.output) result["columnTypes"].push_back(column.type);
                if (maxResultRows_ > 0 && result.contains("rows") && result.at("rows").is_array() && result.at("rows").size() > maxResultRows_)
                    throw MiniSqlError(ErrorCode::Execution, "Result row budget exceeded");
                results.push_back(std::move(result));
            }
            statement.clear();
        });
        return {{"success", true}, {"results", results}, {"statements", results.size()}, {"transactionState", transactionState()}};
    } catch (const MiniSqlError& error) {
        return executionFailure(error, std::move(results));
    } catch (const std::exception& error) {
        return executionFailure(MiniSqlError(ErrorCode::Internal, std::string("Execution failed: ") + error.what()), std::move(results));
    }
}
nlohmann::json Database::executionFailure(const MiniSqlError& error, json results) {
    auto response = error.toJson();
    if (transaction_ == TransactionState::Active) {
        transaction_ = TransactionState::Aborted;
        try {
            rollbackBatch();
            transactionWriteStatements_ = 0;
            for (auto& previous : results) if (previous.value("commitState", "") == "pending") previous["commitState"] = "rolledBack";
            response["transactionRolledBack"] = true;
        } catch (const MiniSqlError& recoveryError) { response = recoveryError.toJson(); }
    }
    response["completedStatements"] = results.size();
    response["results"] = std::move(results);
    response["transactionState"] = transactionState();
    if (unavailable_ || error.code() == ErrorCode::Storage) response["commitState"] = "unknown";
    return response;
}
nlohmann::json Database::executeStreaming(const std::string& source,
                                          const std::function<void(const nlohmann::json&)>& emitMeta,
                                          const std::function<bool(const nlohmann::json&)>& emitRow) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    requireAvailable();
    checkCancelled();
    ActiveDatabaseScope active(this);
    if (transaction_ == TransactionState::Aborted) throw MiniSqlError(ErrorCode::Transaction, "Transaction aborted; ROLLBACK required");
    const auto statements = sql::parse(sql::tokenize(source));
    if (statements.size() != 1 || (statements.front().kind != "Select" && statements.front().kind != "Explain"))
        throw MiniSqlError(ErrorCode::InvalidArgument, "Streaming execution accepts one SELECT or EXPLAIN statement");
    auto plans = sql::compilePlans(statements, catalog_.view());
    materializeSubqueries(plans);
    correlatedRowsCache_.clear();
    const auto& plan = plans.front();
    json columns = json::array(), columnTypes = json::array();
    for (const auto& column : plan.output) {
        columns.push_back(column.name);
        columnTypes.push_back(column.type);
    }
    if (emitMeta) emitMeta({{"columns", std::move(columns)}, {"columnTypes", std::move(columnTypes)}, {"kind", plan.kind}});
    std::size_t emitted = 0;
    try {
        auto stream = openRowStream(plan);
        json row;
        while (stream->next(row)) {
            checkCancelled();
            if (emitRow && !emitRow(row)) throw MiniSqlError(ErrorCode::Cancelled, "Streaming client disconnected");
            ++emitted;
        }
        const auto usage = stream->resourceUsage();
        stream->close();
        return {{"success", true}, {"rows", emitted}, {"resourceUsage", usage}};
    } catch (const MiniSqlError& error) {
        if (error.code() != ErrorCode::InvalidArgument) throw;
    }
    auto result = run(plan);
    for (auto& row : result.at("rows")) {
        checkCancelled();
        if (emitRow && !emitRow(row)) throw MiniSqlError(ErrorCode::Cancelled, "Streaming client disconnected");
        ++emitted;
    }
    return {{"success", true}, {"rows", emitted}, {"resourceUsage", result.value("resourceUsage", json::object())}};
}
nlohmann::json Database::executeScript(const std::string& source, bool optimize) {
    std::lock_guard<std::recursive_mutex> guard(mu_);
    auto response = execute(source, optimize);
    if (transaction_ != TransactionState::Idle && !unavailable_) {
        rollbackBatch();transaction_ = TransactionState::Idle;
        if (response.value("success", false)) {
            const auto results = response.at("results");
            response = MiniSqlError(ErrorCode::Transaction, "Session ended with an uncommitted transaction; rolled back").toJson();
            response["results"] = results;response["completedStatements"] = results.size();
        }
        for (auto& result : response["results"]) if (result.value("commitState", "") == "pending") result["commitState"] = "rolledBack";
        response["transactionState"] = "IDLE";
        response["transactionRolledBack"] = true;
    }
    return response;
}
}
