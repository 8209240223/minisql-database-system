#include "minisql/execution/database.hpp"
#include "minisql/optimizer/optimizer.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace minisql;
int checks = 0;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); ++checks; }
nlohmann::json semanticResult(nlohmann::json value) {
    if (value.is_object()) {
        value.erase("resourceUsage");
        for (auto& item : value.items()) item.value() = semanticResult(std::move(item.value()));
    } else if (value.is_array()) {
        for (auto& item : value) item = semanticResult(std::move(item));
    }
    return value;
}
int main() {
try {
    catalog::Catalog catalog;
    auto parse = [](const std::string& source) { return sql::parse(sql::tokenize(source)); };
    catalog::validate(parse("CREATE TABLE t(id INT,name VARCHAR);"), catalog);
    auto original = sql::compilePlans(parse("SELECT name FROM t WHERE 1=1 AND id>2;"), catalog);
    const auto before = sql::serializePlans(original);
    const auto optimized = optimizer::optimize(original);
    require(optimized.changes.size() == 2, "comparison and boolean rules applied");
    require(optimized.plans[0].children[0].predicate.at("operator") == ">", "constant conjunct removed");
    require(sql::serializePlans(original) == before, "original plan preserved");
    const auto second = optimizer::optimize(optimized.plans);
    require(second.changes.empty(), "optimizer idempotent");
    require(optimized.converged && optimized.iterations == 2, "fixed point confirmed by unchanged structural pass");
    require(second.converged && second.iterations == 1, "stable input converges immediately");
    require(optimized.changes[0]["iteration"] == 1 && optimized.changes[1]["sequence"] == 1, "ordered iteration trace");
    optimizer::Options capped;
    capped.maxIterations = 1;
    auto cappedResult = optimizer::optimize(original, capped);
    require(!cappedResult.converged && cappedResult.diagnostics[0]["code"] == "OPTIMIZER_ITERATION_LIMIT", "iteration exhaustion explicit");
    require(sql::serializePlans(cappedResult.plans) == sql::serializePlans(optimized.plans), "limit retains latest equivalent plan");
    optimizer::Options named;
    named.disabledRules = {"constant-comparison"};
    require(optimizer::optimize(original, named).changes.empty(), "disable by stable rule id");
    named.disabledRules = {"unknown-rule"};
    bool invalidOption = false;
    try { (void)optimizer::optimize(original, named); } catch (const MiniSqlError& e) { invalidOption = e.code() == ErrorCode::InvalidArgument; }
    require(invalidOption, "unknown rule rejected");
    for (auto budget : {std::size_t(0), std::size_t(1), std::size_t(1000001)}) {
        optimizer::Options options; options.maxNodes = budget; invalidOption = false;
        try { (void)optimizer::optimize(original, options); } catch (const MiniSqlError& e) { invalidOption = e.code() == ErrorCode::InvalidArgument; }
        require(invalidOption, "invalid or exhausted node budget rejected");
        require(sql::serializePlans(original) == before, "budget failure preserves caller input");
    }
    for (auto limit : {std::size_t(0), std::size_t(65)}) {
        optimizer::Options options; options.maxIterations = limit; invalidOption = false;
        try { (void)optimizer::optimize(original, options); } catch (const MiniSqlError& e) { invalidOption = e.code() == ErrorCode::InvalidArgument; }
        require(invalidOption, "invalid iteration budget rejected");
    }
    require(optimizer::ruleDescriptors().size() == 9, "built-in rule metadata exposed");
    auto incompatible = sql::compilePlans(parse("SELECT id FROM t WHERE 1=1;"), catalog);
    incompatible[0].children[0].output[0].name = "renamed";
    require(optimizer::optimize(incompatible).plans[0].children[0].kind == "Filter", "filter with different output contract not eliminated");
    require(sql::serializePlans(second.plans) == sql::serializePlans(optimized.plans), "second pass stable");
    optimizer::Options allDisabled;
    allDisabled.disabledRules = {"constant-arithmetic", "constant-comparison", "boolean-simplification", "remove-true-filter", "remove-false-filter", "predicate-pushdown", "hash-join", "prune-columns", "decorrelate-subquery"};
    auto disabled = optimizer::optimize(original, allDisabled);
    require(disabled.changes.empty() && sql::serializePlans(disabled.plans) == before, "all rules disabled");
    auto comparisonOnly = optimizer::optimize(original, {true, false, false});
    require(comparisonOnly.changes.size() == 1 && comparisonOnly.plans[0].children[0].predicate.at("operator") == "AND", "individual rule switch");
    auto trueFilter = optimizer::optimize(sql::compilePlans(parse("SELECT id FROM t WHERE NOT 1!=1;"), catalog));
    require(trueFilter.plans[0].children[0].kind == "SeqScan", "true filter eliminated");
    require(trueFilter.plans[0].output[0].columnId == 0, "projection retained");
    auto falseFilter = optimizer::optimize(sql::compilePlans(parse("SELECT id FROM t WHERE 1=0;"), catalog));
    require(falseFilter.plans[0].children[0].kind == "Limit" && falseFilter.plans[0].children[0].limit == 0, "false filter becomes empty limit");
    require(falseFilter.plans[0].children[0].output[0].name == "id", "false filter preserves output schema");
    require(std::any_of(falseFilter.changes.begin(), falseFilter.changes.end(), [](const auto& change) { return change.at("ruleId") == "remove-false-filter"; }), "false filter rule recorded");
    auto nullFilter = optimizer::optimize(sql::compilePlans(parse("SELECT id FROM t WHERE NULL;"), catalog));
    require(nullFilter.plans[0].children[0].kind == "Limit", "NULL filter becomes empty limit");
    auto pushed = optimizer::optimize(sql::compilePlans(parse("SELECT t.id FROM t JOIN t e ON t.id=e.id WHERE t.id=2;"), catalog));
    require(pushed.plans[0].children[0].kind == "NestedLoopJoin" || pushed.plans[0].children[0].kind == "HashJoin", "join retained after pushdown");
    require(pushed.plans[0].children[0].children[0].kind == "Filter", "left-only predicate pushed into left input");
    require(std::any_of(pushed.changes.begin(), pushed.changes.end(), [](const auto& change) { return change.at("ruleId") == "predicate-pushdown"; }), "pushdown rule recorded");
    auto leftJoin = optimizer::optimize(sql::compilePlans(parse("SELECT t.id FROM t LEFT JOIN t e ON t.id=e.id WHERE t.id=2;"), catalog));
    require(leftJoin.plans[0].children[0].kind == "Filter" && leftJoin.plans[0].children[0].children[0].kind == "LeftJoin" &&
        leftJoin.plans[0].children[0].children[0].children[0].kind != "Filter", "left join filter not pushed by conservative rule");
    auto unsafePush = optimizer::optimize(sql::compilePlans(parse("SELECT t.id FROM t JOIN t e ON t.id=e.id WHERE t.id+1=3;"), catalog));
    require(unsafePush.plans[0].children[0].kind == "Filter" && unsafePush.plans[0].children[0].children[0].children[0].kind != "Filter", "arithmetic predicate not pushed");
    auto hashed = optimizer::optimize(sql::compilePlans(parse("SELECT t.id FROM t JOIN t e ON t.id=e.id;"), catalog));
    require(hashed.plans[0].children[0].kind == "HashJoin", "equality inner join becomes hash join");
    require(std::any_of(hashed.changes.begin(), hashed.changes.end(), [](const auto& change) { return change.at("ruleId") == "hash-join"; }), "hash join rule recorded");
    auto pruned = optimizer::optimize(sql::compilePlans(parse("SELECT name FROM t;"), catalog));
    require(pruned.plans[0].children[0].kind == "SeqScan" && pruned.plans[0].children[0].output.size() == 1 && pruned.plans[0].children[0].output[0].columnId == 1, "unused scan columns pruned");
    require(std::any_of(pruned.changes.begin(), pruned.changes.end(), [](const auto& change) { return change.at("ruleId") == "prune-columns"; }), "column pruning rule recorded");
    auto literalOnly = optimizer::optimize(sql::compilePlans(parse("SELECT 1 FROM t;"), catalog));
    require(literalOnly.plans[0].children[0].output.empty(), "literal-only projection prunes all scan columns");
    const std::vector<std::pair<std::string, nlohmann::json>> truthValues = {
        {"TRUE", true}, {"FALSE", false}, {"NULL", nullptr}
    };
    auto foldedConstant = [&](const std::string& expression, const nlohmann::json& expected, const char* rule) {
        const auto plans = sql::compilePlans(parse("SELECT " + expression + " AS result FROM t;"), catalog);
        const auto result = optimizer::optimize(plans);
        const auto& value = result.plans[0].projections.at(0);
        require(value.at("kind") == "Literal" && value.at("value") == expected && value.at("type") == "bool", "three-valued constant folds with BOOL type");
        require(result.plans[0].output[0].nullable == plans[0].output[0].nullable, "fold preserves nullable output contract");
        require(value.at("line") == plans[0].projections.at(0).at("line") && value.at("column") == plans[0].projections.at(0).at("column"), "constant folding retains expression source location");
        require(optimizer::optimize(result.plans).changes.empty(), "NULL constant folding is idempotent");
        optimizer::Options off; off.disabledRules = {rule};
        const auto disabled = optimizer::optimize(plans, off);
        require(std::none_of(disabled.changes.begin(), disabled.changes.end(), [&](const auto& change) { return change.at("ruleId") == rule; }), "NULL folding honors rule disable switch");
    };
    for (const auto& [leftText, leftValue] : truthValues) {
        foldedConstant("NOT " + leftText, leftValue.is_null() ? nlohmann::json(nullptr) : nlohmann::json(!leftValue.get<bool>()), "boolean-simplification");
        foldedConstant(leftText + " IS NULL", leftValue.is_null(), "boolean-simplification");
        foldedConstant(leftText + " IS NOT NULL", !leftValue.is_null(), "boolean-simplification");
        for (const auto& [rightText, rightValue] : truthValues) {
            const nlohmann::json conjunction = leftValue == false || rightValue == false ? nlohmann::json(false) : leftValue.is_null() || rightValue.is_null() ? nlohmann::json(nullptr) : nlohmann::json(true);
            const nlohmann::json disjunction = leftValue == true || rightValue == true ? nlohmann::json(true) : leftValue.is_null() || rightValue.is_null() ? nlohmann::json(nullptr) : nlohmann::json(false);
            foldedConstant(leftText + " AND " + rightText, conjunction, "boolean-simplification");
            foldedConstant(leftText + " OR " + rightText, disjunction, "boolean-simplification");
        }
    }
    for (const auto& op : {"=", "!=", "<", "<=", ">", ">="}) {
        foldedConstant(std::string("NULL ") + op + " 1", nullptr, "constant-comparison");
        foldedConstant(std::string("1 ") + op + " NULL", nullptr, "constant-comparison");
    }
    bool rejected = false;
    try { optimizer::optimize(sql::compilePlans(parse("SELECT id FROM t WHERE 1=0 AND missing=1;"), catalog)); }
    catch (const MiniSqlError& e) { rejected = e.code() == ErrorCode::Semantic; }
    require(rejected, "optimization cannot hide invalid name");
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("optimizer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    execution::Database database(directory / "database.pages", 2);
    require(database.execute("CREATE TABLE t(id INT,name VARCHAR); INSERT INTO t(id,name) VALUES(1,'a'); INSERT INTO t(id,name) VALUES(2,'b'); INSERT INTO t(id,name) VALUES(2,'b');")["success"] == true, "fixture persisted");
    const auto joinPlain = database.execute("SELECT t.id,e.id FROM t JOIN t e ON t.id=e.id ORDER BY t.id,e.id;", false);
    const auto joinFast = database.execute("SELECT t.id,e.id FROM t JOIN t e ON t.id=e.id ORDER BY t.id,e.id;", true);
    require(joinPlain["success"] == true && semanticResult(joinFast) == semanticResult(joinPlain), "hash join results equal nested loop results including duplicates");
    const auto prunePlain = database.execute("SELECT name FROM t ORDER BY name;", false);
    const auto pruneFast = database.execute("SELECT name FROM t ORDER BY name;", true);
    require(prunePlain["success"] == true && semanticResult(pruneFast) == semanticResult(prunePlain), "column pruning preserves projected rows");
    const auto literalPlain = database.execute("SELECT 1 FROM t;", false);
    const auto literalFast = database.execute("SELECT 1 FROM t;", true);
    require(literalPlain["success"] == true && semanticResult(literalFast) == semanticResult(literalPlain), "all-column pruning preserves literal projection cardinality");
    for (const auto& [leftText, leftValue] : truthValues) {
        for (const auto& [rightText, rightValue] : truthValues) {
            for (const auto& op : {"AND", "OR"}) {
                const bool absorbing = std::string(op) == "OR";
                const nlohmann::json expected = leftValue == absorbing || rightValue == absorbing ? nlohmann::json(absorbing) : leftValue.is_null() || rightValue.is_null() ? nlohmann::json(nullptr) : nlohmann::json(!absorbing);
                const auto source = "SELECT " + leftText + " " + op + " " + rightText + " AS result FROM t;";
                const auto plain = database.execute(source, false);
                require(plain.at("success") == true && plain.at("results")[0].at("rows") == nlohmann::json::array({{expected},{expected},{expected}}), "executor matches independent three-valued truth table including duplicates");
                require(database.execute(source, true) == plain, "folded truth table matches unoptimized execution");
            }
        }
    }
    for (const auto& expression : {"NULL AND 1/0=1", "NULL OR CAST('bad' AS INT)=1", "NULL=1/0", "1/0=NULL", "(1/0=1) AND FALSE", "(1/0=1) OR TRUE", "CAST('bad' AS INT) IS NULL", "CAST('bad' AS INT) IS NOT NULL"}) {
        const auto source = std::string("SELECT ") + expression + " FROM t;";
        const auto plain = database.execute(source, false);
        require(plain.at("success") == false && plain.at("error").at("code") == 5001 && database.execute(source, true) == plain, "NULL folding retains evaluated errors and locations");
    }
    for (const auto& source : {"SELECT COUNT(*),SUM(id) FROM t WHERE NULL=1;", "SELECT COUNT(*) FROM t HAVING NOT NULL;", "SELECT 1/0 FROM t WHERE NULL AND FALSE;", "SELECT id FROM t WHERE NULL OR TRUE;", "BEGIN; UPDATE t SET id=1/0 WHERE NULL=NULL; DELETE FROM t WHERE NOT NULL; ROLLBACK;"}) {
        const auto plain = database.execute(source, false);
        const auto fast = database.execute(source, true);
        require(plain.at("success") == true && semanticResult(fast) == semanticResult(plain),
                "NULL folding preserves aggregate empty input and DML no-op behavior");
    }
    for (const auto& predicate : {"1=1 AND id>1", "1=0 OR id=1", "NOT 1!=1", "id=1 AND 1=1", "id=1 OR 1=0",
                                 "1=0 AND id=1", "1=1 OR id=1", "'a'<'b' AND id=2", "NOT (1=1 AND 2=2)",
                                 "(1=1 AND id=2) OR (1=0 AND id=1)", "id=1 AND 1=0", "id=1 OR 1=1"}) {
        const auto source = std::string("SELECT name,id,name FROM t WHERE ") + predicate + ";";
        const auto plain = database.execute(source, false);
        const auto fast = database.execute(source, true);
        require(plain["success"] == true && semanticResult(fast) == semanticResult(plain), "optimized results equal including duplicates");
    }
    auto compiled = database.compile("SELECT id FROM t WHERE 1=1;");
    require(compiled["plan"].size() == 3 && compiled["optimizedPlan"].size() == 2, "compile exposes both plans");
    auto deletion = database.execute("DELETE FROM t WHERE 1=0 OR id=1;");
    require(deletion["success"] == true && deletion["results"][0]["affectedRows"] == 1, "optimized delete count");
    require(database.execute("SELECT * FROM t;")["results"][0]["rows"].size() == 2, "optimized delete retains duplicate rows");
    for (const auto& predicate : {"1+2*3=7", "(1+2)*3=9", "-7/2=-3", "-(id+1)=-3", "+id=2",
                                 "id+1*2=4", "1=0 AND 1/0=1", "1=1 OR 2147483647+1=0", "NOT id+1<=2"}) {
        const auto source = std::string("SELECT id FROM t WHERE ") + predicate + ";";
        const auto plain = database.execute(source, false);
        const auto optimizedResult = database.execute(source, true);
        require(plain["success"] == true && semanticResult(optimizedResult) == semanticResult(plain), "arithmetic precedence and short circuit equivalence");
    }
    for (const auto& predicate : {"1/0=1", "2147483647+1=0", "-2147483648/-1=0", "-(-2147483648)=0",
                                 "50000*50000=0", "1/0=1 AND 1=0", "1/0=1 OR 1=1"}) {
        const auto source = std::string("SELECT id FROM t WHERE ") + predicate + ";";
        const auto plain = database.execute(source, false);
        const auto optimizedResult = database.execute(source, true);
        require(plain["success"] == false && plain["error"]["code"] == 5001 && semanticResult(optimizedResult) == semanticResult(plain), "arithmetic runtime error preserved");
        require(database.compile(source)["success"] == true, "constant runtime failure not raised at compile time");
    }
    auto arithmeticPlan = database.compile("SELECT id FROM t WHERE id>10+8;");
    require(arithmeticPlan["optimizationRules"][0]["ruleId"] == "constant-arithmetic", "arithmetic rule recorded");
    require(database.execute("SELECT id FROM t WHERE id+'bad'=2;")["error"]["code"] == 2003, "arithmetic types rejected");
    const auto projected = database.execute("SELECT id+2*3 AS adjusted,name AS label,id=2 AS matched FROM t;");
    require(projected["success"] == true, "expression projection accepted");
    require(projected["results"][0]["columns"] == nlohmann::json::array({"adjusted","label","matched"}), "projection aliases");
    require(projected["results"][0]["rows"][0] == nlohmann::json::array({8,"b",true}), "computed values and boolean projection");
    require(projected["results"][0]["rows"].size() == 2, "computed duplicate rows retained");
    require(database.execute("SELECT id AS x,id+1 AS x FROM t;")["results"][0]["columns"] == nlohmann::json::array({"x","x"}), "duplicate aliases preserved");
    require(database.execute("SELECT id+1 FROM t;")["results"][0]["columns"][0] == "expr_1", "stable generated expression name");
    require(database.execute("SELECT id+1 AS adjusted FROM t WHERE adjusted>2;")["error"]["code"] == 2003, "alias not visible in WHERE");
    require(database.execute("SELECT missing+1 AS x FROM t;")["error"]["code"] == 2003, "missing expression column rejected");
    require(database.execute("SELECT name+1 FROM t;")["error"]["code"] == 2003, "invalid expression type rejected");
    for (const auto& source : {"SELECT 1+2*3 AS result FROM t;", "SELECT 1/0 AS error FROM t;", "SELECT 1/0 FROM t WHERE 1=0;"}) {
        require(semanticResult(database.execute(source, false)) == semanticResult(database.execute(source, true)), "projection optimization preserves values and errors");
    }
    require(database.execute("INSERT INTO t(id,name) VALUES(2,'c'); INSERT INTO t(id,name) VALUES(3,'b');")["success"] == true, "distinct fixture");
    auto distinct = database.execute("SELECT DISTINCT id,name FROM t;");
    require(distinct["success"] == true && distinct["results"][0]["rows"].size() == 3, "distinct uses whole row");
    distinct = database.execute("SELECT DISTINCT id FROM t;");
    require(distinct["results"][0]["rows"].size() == 2, "distinct uses projected columns");
    distinct = database.execute("SELECT DISTINCT 1+1 AS fixed,name AS label FROM t;");
    require(distinct["results"][0]["rows"].size() == 2, "distinct expression results");
    require(semanticResult(distinct) == semanticResult(database.execute("SELECT DISTINCT 1+1 AS fixed,name AS label FROM t;", false)), "distinct optimization equivalence");
    distinct = database.execute("SELECT DISTINCT name,name FROM t;");
    require(distinct["results"][0]["columns"] == nlohmann::json::array({"name","name"}) && distinct["results"][0]["rows"].size() == 2, "distinct duplicate output columns");
    distinct = database.execute("SELECT DISTINCT * FROM t WHERE 1=0;");
    require(distinct["results"][0]["rows"].empty() && distinct["results"][0]["columns"].size() == 2, "empty distinct preserves schema");
    require(database.execute("SELECT DISTINCT DISTINCT id FROM t;")["success"] == false, "repeated DISTINCT rejected");
    const auto distinctPlan = database.compile("SELECT DISTINCT name FROM t;")["plan"];
    require(distinctPlan[0]["kind"] == "Distinct" && distinctPlan[1]["kind"] == "Project", "distinct above projection");
    require(database.execute("SELECT id FROM t LIMIT 1;")["results"][0]["rows"].size() == 1, "limit count");
    require(database.execute("SELECT id FROM t LIMIT 2 OFFSET 3;")["results"][0]["rows"].size() == 1, "offset near end");
    require(database.execute("SELECT id FROM t OFFSET 2;")["results"][0]["rows"].size() == 2, "offset without limit");
    auto emptyPage = database.execute("SELECT id AS code FROM t LIMIT 0;")["results"][0];
    require(emptyPage["rows"].empty() && emptyPage["columns"][0] == "code", "zero limit preserves schema");
    require(database.execute("SELECT id FROM t LIMIT 18446744073709551615 OFFSET 18446744073709551615;")["results"][0]["rows"].empty(), "large counts do not overflow");
    require(database.execute("SELECT id FROM t LIMIT 18446744073709551615;")["results"][0]["rows"].size() == 4, "maximum limit accepted");
    require(database.execute("SELECT DISTINCT id FROM t LIMIT 1 OFFSET 1;")["results"][0]["rows"].size() == 1, "pagination after distinct");
    const auto pagePlan = database.compile("SELECT DISTINCT id FROM t LIMIT 2;")["plan"];
    require(pagePlan[0]["kind"] == "Limit" && pagePlan[1]["kind"] == "Distinct", "limit node above distinct");
    for (const auto& suffix : {"LIMIT -1", "LIMIT 1.5", "LIMIT '2'", "LIMIT 18446744073709551616", "LIMIT 1 OFFSET -1", "LIMIT 1 LIMIT 2", "OFFSET 1 LIMIT 2"}) {
        require(database.execute(std::string("SELECT id FROM t ") + suffix + ";")["success"] == false, "invalid pagination rejected");
    }
    auto ordered = database.execute("SELECT id,name FROM t ORDER BY id DESC,name ASC;");
    require(ordered["success"] == true && ordered["results"][0]["rows"] == nlohmann::json::array({{3,"b"},{2,"b"},{2,"b"},{2,"c"}}), "mixed multi-key sort");
    ordered = database.execute("SELECT name FROM t ORDER BY id DESC,name ASC;");
    require(ordered["results"][0]["rows"] == nlohmann::json::array({{"b"},{"b"},{"b"},{"c"}}), "hidden source sort column removed");
    require(ordered["results"][0]["columns"] == nlohmann::json::array({"name"}), "hidden sort column not in schema");
    ordered = database.execute("SELECT id+1 AS total,name FROM t ORDER BY total DESC,name DESC LIMIT 2 OFFSET 1;");
    require(ordered["success"] == true && ordered["results"][0]["rows"] == nlohmann::json::array({{3,"c"},{3,"b"}}), "alias sort then pagination");
    ordered = database.execute("SELECT DISTINCT id FROM t ORDER BY id DESC LIMIT 1;");
    require(ordered["results"][0]["rows"] == nlohmann::json::array({{3}}), "distinct sort limit composition");
    ordered = database.execute("SELECT DISTINCT id+1 AS total FROM t ORDER BY total DESC;");
    require(ordered["results"][0]["rows"] == nlohmann::json::array({{4},{3}}), "distinct expression alias sort");
    require(database.execute("SELECT DISTINCT name FROM t ORDER BY id;")["error"]["code"] == 2003, "distinct hidden key rejected");
    require(database.execute("SELECT id AS x,name AS x FROM t ORDER BY x;")["error"]["code"] == 2003, "ambiguous output alias rejected");
    require(database.execute("SELECT id+1 AS id FROM t ORDER BY id;")["error"]["code"] == 2003, "source alias conflict rejected");
    require(database.execute("SELECT name FROM t ORDER BY missing;")["error"]["code"] == 2003, "unknown order column rejected");
    const auto sortSql = "SELECT name FROM t WHERE 1=1 ORDER BY id+2 DESC,name LIMIT 2;";
    require(semanticResult(database.execute(sortSql, true)) == semanticResult(database.execute(sortSql, false)), "sort optimizer equivalence");
    require(database.execute("SELECT * FROM t WHERE 1=0 ORDER BY id;")["results"][0]["rows"].empty(), "empty sorted output");
    const auto sortedPlan = database.compile("SELECT DISTINCT id FROM t ORDER BY id LIMIT 1;")["plan"];
    require(sortedPlan[0]["kind"] == "Limit" && sortedPlan[1]["kind"] == "Sort" && sortedPlan[2]["kind"] == "Distinct", "explicit sort plan order");
    for (const auto& predicate : {"a.id=b.id", "1=1 AND a.id=b.id", "1=0 OR a.id=b.id", "1=0 AND 1/0=1", "1/0=1 AND a.id=b.id", "a.id+1=b.id+1"}) {
        const auto source = std::string("SELECT a.id,b.id FROM t a JOIN t b ON ") + predicate + " ORDER BY a.id,b.id;";
        require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "join optimizer preserves rows and runtime errors");
    }
    for (const auto& expression : {"NULL=NULL", "NULL!=1", "NOT NULL", "TRUE AND NULL", "NULL AND TRUE", "FALSE OR NULL", "NULL OR FALSE", "NULL AND FALSE", "NULL OR TRUE", "NULL+1", "NULL IS NULL", "NULL IS NOT NULL", "NULL AND 1/0=1", "FALSE AND 1/0=1"}) {
        const auto source = std::string("SELECT ") + expression + " FROM t;";
        require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "NULL optimization preserves truth values and errors");
    }
    for (const auto& expression : {"CAST(1+2 AS BIGINT)", "CAST('9223372036854775807' AS BIGINT)",
             "CAST(NULL AS INT)", "CAST('bad' AS INT)", "FALSE AND CAST('bad' AS INT)=1",
             "TRUE OR CAST('bad' AS INT)=1", "CAST('bad' AS INT)=1 AND FALSE",
             "CAST(2147483648 AS INT)", "CAST(CAST(id AS VARCHAR) AS BIGINT)"}) {
        const auto source = std::string("SELECT ") + expression + " FROM t;";
        require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "CAST optimization preserves values and evaluation errors");
    }
    for (const auto& values : {"CAST('42' AS INT),CAST(1+2 AS VARCHAR)", "1/0,'bad'",
             "CAST('bad' AS INT),'bad'", "CAST(2147483648 AS INT),'bad'", "NULL,CAST(NULL AS VARCHAR)"}) {
        const auto source = std::string("INSERT INTO t(id,name) VALUES(") + values + ");";
        require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "INSERT expressions preserve result and error with optimization");
    }
    for (const auto& values : {"(1+2,'a'),(3*4,'b')", "(5,'a'),(1/0,'b')",
             "(5,'a'),(CAST('bad' AS INT),'b')", "(NULL,NULL),(7,CAST(8 AS VARCHAR))"}) {
        require(database.execute("DELETE FROM t;")["success"] == true, "clear optimized multirow fixture");
        const auto source = std::string("INSERT INTO t(id,name) VALUES") + values + ";";
        const auto multirowOptimized = database.execute(source, true);
        const auto optimizedRows = database.execute("SELECT * FROM t ORDER BY id;");
        require(database.execute("DELETE FROM t;")["success"] == true, "clear plain multirow fixture");
        require(semanticResult(multirowOptimized) == semanticResult(database.execute(source, false)), "multirow optimization preserves errors and counts");
        require(optimizedRows == database.execute("SELECT * FROM t ORDER BY id;"), "multirow optimization preserves persisted rows");
    }
    auto aggregatePlans = sql::compilePlans(parse("SELECT SUM(1+2),COUNT(*) FROM t GROUP BY id+1 HAVING TRUE;"), catalog);
    const auto aggregateBefore = sql::serializePlans(aggregatePlans);
    const auto aggregateOptimized = optimizer::optimize(aggregatePlans);
    require(sql::serializePlans(aggregatePlans) == aggregateBefore, "aggregate optimization preserves input plan");
    require(aggregateOptimized.plans[0].children[0].kind == "Aggregate", "true HAVING removed without removing aggregate");
    require(aggregateOptimized.plans[0].children[0].aggregates[0]["argument"]["value"] == 3, "aggregate argument folded");
    require(optimizer::optimize(aggregateOptimized.plans).changes.empty(), "aggregate optimizer reaches fixed point");
    for (bool groupBudget : {false, true}) {
        auto oversized = aggregateOptimized.plans;
        auto& node = oversized[0].children[0];
        for (std::size_t i = 0; i < 256; ++i) {
            if (groupBudget) node.groupKeys.push_back(node.groupKeys[0]);
            else node.aggregates.push_back(node.aggregates[0]);
        }
        const auto preserved = sql::serializePlans(oversized);
        optimizer::Options options;options.maxNodes = 500;
        bool exhausted = false;
        try { (void)optimizer::optimize(oversized, options); }
        catch (const MiniSqlError& error) { exhausted = error.code() == ErrorCode::InvalidArgument; }
        require(exhausted, "aggregate and grouping expressions count toward optimizer budget");
        require(sql::serializePlans(oversized) == preserved, "aggregate budget failure preserves input");
    }
    for (const auto& source : {
        "SELECT COUNT(*),SUM(id),MIN(id),MAX(id) FROM t;",
        "SELECT name,COUNT(*) FROM t GROUP BY name HAVING COUNT(*)>0 ORDER BY name;",
        "SELECT COUNT(*) FROM t WHERE FALSE HAVING COUNT(*)=0;",
        "SELECT id+1,COUNT(*) FROM t WHERE TRUE GROUP BY id ORDER BY id;",
        "SELECT SUM(1+2),COUNT(*) FROM t GROUP BY name HAVING TRUE;",
        "SELECT DISTINCT COUNT(*) FROM t GROUP BY name ORDER BY COUNT(*);",
        "SELECT COUNT(*) FROM t WHERE FALSE GROUP BY 1/0;",
        "SELECT SUM(1/0) FROM t;",
        "SELECT COUNT(*) FROM t HAVING COUNT(*)/0>0;",
        "SELECT MIN(id>0),MAX(id>0) FROM t;",
        "SELECT SUM(CAST(name AS INT)) FROM t;"
    }) require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "aggregate optimizer preserves results and errors");
    for (const auto& source : {
        "SELECT AVG(id) FROM t;", "SELECT -AVG(id),+AVG(id) FROM t;",
        "SELECT name,AVG(id) FROM t GROUP BY name HAVING AVG(id)>0 ORDER BY AVG(id);",
        "SELECT AVG(id) FROM t WHERE FALSE;", "SELECT DISTINCT AVG(id) FROM t GROUP BY name ORDER BY AVG(id);",
        "SELECT AVG(1+2) FROM t;", "SELECT AVG(1/0) FROM t;",
        "SELECT CAST(AVG(id) AS INT),CAST(AVG(id) AS VARCHAR) FROM t;",
        "SELECT AVG(id)+1,AVG(id)-1,AVG(id)*2,AVG(id)/3 FROM t;",
        "SELECT AVG(id)*AVG(id),-(AVG(id)*AVG(id)) FROM t;",
        "SELECT name FROM t GROUP BY name HAVING AVG(id)*AVG(id)>0 ORDER BY AVG(id)*AVG(id);",
        "SELECT AVG(id)/0 FROM t;", "SELECT FALSE AND AVG(id)/0>0,TRUE OR AVG(id)/0>0 FROM t;",
        "SELECT AVG(id)*AVG(id)/AVG(id) FROM t;",
        "SELECT AVG(id)*AVG(id),AVG(id)/0 FROM t WHERE FALSE;",
        "SELECT CAST(AVG(id)*AVG(id) AS INT),CAST(AVG(id)*AVG(id) AS VARCHAR) FROM t;"
    }) require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "exact AVG optimizer preserves results and errors");
    for (const auto& source : {
        "SELECT 0.1+0.2,1.25*0.125,1.0/3.0 FROM t;",
        "SELECT 2.0<10.0,-10.0<-2.0,1.0=1.00,1.0=1,1.0!=2.0 FROM t;",
        "SELECT FALSE AND 1.0/0.0>0,TRUE OR 1.0/0.0>0 FROM t;",
        "SELECT 1.0/0.0 FROM t;",
        "SELECT 9999999999999999999999999999999999999.9+0.1 FROM t;",
        "SELECT -(1.2+3.40),CAST(2.5 AS INT),CAST(-2.5 AS BIGINT) FROM t;",
        "SELECT MIN(id+0.1),MAX(id+0.1),SUM(id+0.1),AVG(id+0.1) FROM t;",
        "SELECT AVG(0.00000001),SUM(0.00000001) FROM t;",
        "SELECT id+0.5,COUNT(*) FROM t GROUP BY id+00.5 ORDER BY id+0.5;",
        "SELECT DISTINCT id+0.1 FROM t ORDER BY id+0.1;"
    }) require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "decimal literals preserve results and errors under optimization");
    for (const auto& source : {
        "SELECT CAST(1.235 AS DECIMAL(4,2)),CAST(-1.235 AS DECIMAL(4,2)) FROM t;",
        "SELECT CAST(1 AS DECIMAL(4,2))+0.1,CAST(2 AS DECIMAL(4,2))/3 FROM t;",
        "SELECT CAST('bad' AS DECIMAL(2,1)) FROM t;",
        "SELECT FALSE AND CAST('bad' AS DECIMAL(2,1))>0 FROM t;",
        "SELECT CAST(9.995 AS DECIMAL(3,2)) FROM t;",
        "SELECT SUM(CAST(id AS DECIMAL(12,2))),AVG(CAST(id AS DECIMAL(12,2))) FROM t;",
        "SELECT CAST(id AS DECIMAL(12,2)) FROM t GROUP BY CAST(id AS DECIMAL(012,02)) ORDER BY CAST(id AS DECIMAL(12,2));",
        "SELECT CAST(NULL AS DECIMAL(2,1)),CAST(CAST(-2.5 AS DECIMAL(2,1)) AS INT) FROM t;"
    }) require(semanticResult(database.execute(source, true)) == semanticResult(database.execute(source, false)), "DECIMAL target CAST optimizer equivalence");
    const auto varcharFixture=database.execute("CREATE TABLE varchar_values(id INT,s VARCHAR(2)); INSERT INTO varchar_values VALUES(1,'ab'),(2,''),(3,NULL);",true);
    require(varcharFixture.at("success")==true,"VARCHAR optimizer fixture");
    for(const auto* source : {
        "SELECT s FROM varchar_values WHERE s='ab' ORDER BY s;",
        "SELECT MIN(s),MAX(s),COUNT(s) FROM varchar_values;",
        "SELECT s,COUNT(*) FROM varchar_values GROUP BY s ORDER BY s;",
        "SELECT CAST(s AS VARCHAR(2)) FROM varchar_values GROUP BY CAST(s AS VARCHAR(02)) ORDER BY CAST(s AS VARCHAR(2));",
        "SELECT FALSE AND CAST('long' AS VARCHAR(1))='x' FROM varchar_values;",
        "SELECT CAST('long' AS VARCHAR(1)) FROM varchar_values;",
        "SELECT CAST(1.20 AS VARCHAR(4)),CAST(TRUE AS VARCHAR(4)) FROM varchar_values;",
        "BEGIN; UPDATE varchar_values SET s='xy'; SELECT s FROM varchar_values ORDER BY id; ROLLBACK;"
    }) require(semanticResult(database.execute(source,true))==semanticResult(database.execute(source,false)),"VARCHAR optimizer equivalence");
    require(database.execute("CREATE TABLE date_values(id INT,d DATE); INSERT INTO date_values VALUES(1,DATE '1999-12-31'),(2,DATE '2000-02-29'),(3,NULL);",true).at("success") == true,"DATE optimizer fixture");
    for(const auto* source : {
        "SELECT d FROM date_values WHERE d>=DATE '2000-01-01' ORDER BY d;",
        "SELECT MIN(d),MAX(d),COUNT(d) FROM date_values;",
        "SELECT d,COUNT(*) FROM date_values GROUP BY d ORDER BY d;",
        "SELECT DATE '2000-02-29',COUNT(*) FROM date_values GROUP BY date '2000-02-29';",
        "SELECT FALSE AND CAST('bad' AS DATE)=DATE '2000-01-01' FROM date_values;",
        "SELECT CAST('bad' AS DATE) FROM date_values;",
        "SELECT CAST(CAST(d AS VARCHAR) AS DATE) FROM date_values ORDER BY id;",
        "BEGIN; UPDATE date_values SET d=DATE '2024-02-29'; SELECT * FROM date_values ORDER BY id; ROLLBACK;"
    }) require(semanticResult(database.execute(source,true))==semanticResult(database.execute(source,false)),"DATE optimizer equivalence");
    require(database.execute("CREATE TABLE bool_values(id INT,b BOOL); INSERT INTO bool_values VALUES(1,TRUE),(2,FALSE),(3,NULL);",true).at("success") == true, "persistent BOOL optimizer fixture");
    for (const auto* source : {
        "SELECT b,NOT b,b AND NULL,b OR NULL FROM bool_values ORDER BY id;",
        "SELECT id FROM bool_values WHERE TRUE AND b ORDER BY id;",
        "SELECT id FROM bool_values WHERE FALSE OR NOT b ORDER BY id;",
        "SELECT COUNT(b),MIN(b),MAX(b) FROM bool_values;",
        "SELECT b,COUNT(*) FROM bool_values GROUP BY b ORDER BY b;",
        "SELECT FALSE AND CAST('bad' AS BOOL),TRUE OR CAST('bad' AS BOOL) FROM bool_values;",
        "SELECT CAST('bad' AS BOOL) FROM bool_values;",
        "SELECT CAST(CAST(b AS VARCHAR) AS BOOL) FROM bool_values ORDER BY id;",
        "BEGIN; UPDATE bool_values SET b=NOT b; SELECT b FROM bool_values ORDER BY id; ROLLBACK;"
    }) require(semanticResult(database.execute(source,true)) == semanticResult(database.execute(source,false)), "persistent BOOL optimizer equivalence");
    const auto decimalFixture = database.execute("CREATE TABLE decimal_values(id INT,v DECIMAL(12,2)); INSERT INTO decimal_values VALUES(1,0.1),(2,10.0),(3,-2.5),(4,NULL);",true);
    require(decimalFixture.at("success") == true, "persistent DECIMAL optimizer fixture");
    for (const auto& source : {
        "SELECT * FROM decimal_values ORDER BY v;",
        "SELECT v+0.1,v*0.5 FROM decimal_values WHERE v<10.0;",
        "SELECT SUM(v),AVG(v),MIN(v),MAX(v) FROM decimal_values;",
        "SELECT v,COUNT(*) FROM decimal_values GROUP BY v HAVING COUNT(*)>0 ORDER BY v;",
        "SELECT DISTINCT v FROM decimal_values ORDER BY v DESC;",
        "SELECT a.v,b.v FROM decimal_values a LEFT JOIN decimal_values b ON a.v=b.v ORDER BY a.id;",
        "SELECT CAST(v AS DECIMAL(5,1)) FROM decimal_values ORDER BY id;",
        "BEGIN; UPDATE decimal_values SET v=CAST(v+1 AS DECIMAL(12,2)); SELECT v FROM decimal_values ORDER BY id; ROLLBACK;"
    }) require(semanticResult(database.execute(source,true)) == semanticResult(database.execute(source,false)), "persistent DECIMAL optimizer equivalence");
    std::cout << checks << " optimizer checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "optimizer contract failed after " << checks << " checks: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "optimizer contract failed after " << checks << " checks with an unknown exception\n";
    return 1;
}
}
