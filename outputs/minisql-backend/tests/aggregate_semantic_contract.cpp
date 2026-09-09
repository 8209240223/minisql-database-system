#include "minisql/catalog/catalog.hpp"
#include <iostream>
#include <stdexcept>

namespace {
std::size_t checks = 0;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
auto parse(const std::string& source) { return minisql::sql::parse(minisql::sql::tokenize(source)); }
}
int main() {
    using namespace minisql;
    catalog::Catalog base;
    catalog::validate(parse("CREATE TABLE t(id INT,v INT,b BIGINT,s VARCHAR); CREATE TABLE r(id INT,v INT);"), base);
    const auto analyze = [&](const std::string& source) {
        auto copy = base;
        const auto statements = parse(source);
        catalog::validate(statements, copy);
        const auto& statement = statements.at(0);
        return catalog::analyzeSelect(statement, catalog::queryScope(statement, copy));
    };
    for (const auto& source : {
        "SELECT COUNT(*),COUNT(v),SUM(v),AVG(v),MIN(v),MAX(v) FROM t;",
        "SELECT v,COUNT(*) FROM t GROUP BY v;",
        "SELECT t.v,COUNT(*) FROM t GROUP BY V;",
        "SELECT v+1,COUNT(*) FROM t GROUP BY v;",
        "SELECT (v+1)*2,COUNT(*) FROM t GROUP BY t.v+1;",
        "SELECT (v+01)*2,COUNT(*) FROM t GROUP BY t.v+1;",
        "SELECT (v+1)*2,COUNT(*) FROM t GROUP BY t.v+01;",
        "SELECT v+0,COUNT(*) FROM t GROUP BY v+-0;",
        "SELECT v,id,COUNT(*) FROM t GROUP BY id,v HAVING COUNT(*)>1;",
        "SELECT COUNT(*) FROM t HAVING AVG(v)>1 AND SUM(v)>0;",
        "SELECT 1 FROM t HAVING TRUE;",
        "SELECT COUNT(*)+SUM(v),MAX(s) FROM t WHERE v>0 HAVING MIN(v) IS NOT NULL;",
        "SELECT v AS k,COUNT(*) AS n FROM t GROUP BY v ORDER BY n DESC,k;",
        "SELECT COUNT(*) FROM t ORDER BY SUM(v);",
        "SELECT * FROM t GROUP BY id,v,b,s;",
        "SELECT x.*,COUNT(*) FROM t x GROUP BY x.id,x.v,x.b,x.s;",
        "SELECT x.id,COUNT(y.id) FROM t x LEFT JOIN r y ON x.id=y.id GROUP BY x.id;",
        "SELECT COUNT(*) FROM t x JOIN r y ON x.id=y.id HAVING COUNT(y.v)>0;",
        "SELECT COUNT(v>0),MIN(v>0),MAX(s),SUM(NULL),AVG(NULL),MIN(NULL) FROM t;",
        "SELECT DISTINCT v,COUNT(*) FROM t GROUP BY v ORDER BY v LIMIT 1 OFFSET 1;",
        "SELECT v FROM t GROUP BY v HAVING NULL;",
        "SELECT SUM(CAST(v AS BIGINT)+1) FROM t;",
    }) require(analyze(source).aggregated, source);
    const auto types = analyze("SELECT COUNT(*),COUNT(s),SUM(v),SUM(b),AVG(v),AVG(b),MIN(v),MAX(s),MIN(NULL) FROM t;");
    require(types.projectionTypes == std::vector<std::string>{"bigint","bigint","bigint","bigint","decimal(38,6)","decimal(38,6)","int","varchar","null"}, "aggregate result types");
    require(analyze("SELECT v FROM t GROUP BY v,s;").groupTypes == std::vector<std::string>{"int","varchar"}, "group types");
    require(!analyze("SELECT v FROM t ORDER BY v;").aggregated, "ordinary query is not aggregated");
    require(analyze("SELECT * FROM t;").projectionTypes == std::vector<std::string>{"int","int","bigint","varchar"}, "wildcard output types");
    for (const auto& source : {
        "SELECT id,COUNT(*) FROM t;",
        "SELECT v FROM t GROUP BY v+1;",
        "SELECT v+id FROM t GROUP BY v;",
        "SELECT * FROM t GROUP BY id;",
        "SELECT v AS k FROM t GROUP BY k;",
        "SELECT COUNT(*) AS n FROM t HAVING n>0;",
        "SELECT COUNT(*) FROM t HAVING v>0;",
        "SELECT COUNT(*) FROM t HAVING COUNT(*);",
        "SELECT COUNT(*) FROM t ORDER BY v;",
        "SELECT COUNT(*) AS v FROM t ORDER BY v;",
        "SELECT v FROM t ORDER BY SUM(v);",
        "SELECT SUM(s) FROM t;",
        "SELECT AVG(s) FROM t;",
        "SELECT SUM(v>0) FROM t;",
        "SELECT COUNT(missing) FROM t;",
        "SELECT SUM(COUNT(*)) FROM t;",
        "SELECT COUNT(v+SUM(v)) FROM t;",
        "SELECT AVG(MAX(v)) FROM t;",
        "SELECT v FROM t WHERE COUNT(*)>0;",
        "SELECT COUNT(*) FROM t GROUP BY SUM(v);",
        "SELECT COUNT(*) FROM t JOIN r ON COUNT(*)>0;",
        "SELECT COUNT(id) FROM t JOIN r ON t.id=r.id;",
        "SELECT t.id,COUNT(*) FROM t JOIN r ON t.id=r.id GROUP BY r.id;",
        "SELECT COUNT(t.id) FROM t x;",
        "SELECT v+'A' FROM t GROUP BY v+'a';",
        "INSERT INTO t(v) VALUES(COUNT(*));",
        "UPDATE t SET v=SUM(v);",
        "DELETE FROM t WHERE COUNT(*)>0;",
        "CREATE TABLE invalid(v INT CHECK(COUNT(*)>0));",
    }) {
        bool rejected = false;
        try { auto copy = base;catalog::validate(parse(source), copy); }
        catch (const MiniSqlError& error) { rejected = error.code() == ErrorCode::Semantic; }
        require(rejected, std::string("Expected semantic error: ") + source);
    }
    try { (void)analyze("SELECT\nSUM(COUNT(*)) FROM t;");require(false, "nested aggregate accepted"); }
    catch (const MiniSqlError& error) {
        require(error.code() == ErrorCode::Semantic, "nested error type");
        require(error.location().line == 2 && error.location().column == 5, "nested aggregate error location");
    }
    require(base.find("invalid") == nullptr, "failed CHECK does not publish catalog entry");
    require(analyze("SELECT AVG(v)*AVG(v) FROM t;").projectionTypes[0] == "decimal(38,12)", "multiplication adds scale");
    require(analyze("SELECT AVG(v)+1 FROM t;").projectionTypes[0] == "decimal(38,6)", "addition preserves scale");
    require(analyze("SELECT AVG(v)*AVG(v)/AVG(v) FROM t;").projectionTypes[0] == "decimal(38,12)", "division preserves greater scale");
    bool scaleRejected = false;
    try { (void)analyze("SELECT AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v)*AVG(v) FROM t;"); }
    catch (const MiniSqlError& error) { scaleRejected = error.code() == ErrorCode::Semantic; }
    require(scaleRejected, "multiplication scale overflow must not round away digits");
    std::cout << checks << " aggregate semantic checks passed\n";
}
