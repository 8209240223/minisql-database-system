// X09 Phase 3.1 contract: every parsed subquery expression (EXISTS / IN / scalar)
// must carry a structured statement node (Expr::subquery) alongside the interim
// SQL text (Expr::subquerySql). This verifies migration of object identity
// is in place before the planner/optimizer switch over in 3.3-3.5.
#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"
#include "minisql/sql/planner.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace {
using namespace minisql::sql;

// Depth-first traversal that yields every expression reaching a subquery node.
void findSubqueries(const std::shared_ptr<Expr>& e,
                    std::vector<std::shared_ptr<Expr>>& out) {
    if (!e) return;
    if (e->kind == "InSubquery" || e->kind == "Exists" || e->kind == "ScalarSubquery")
        out.push_back(e);
    findSubqueries(e->left, out);
    findSubqueries(e->right, out);
}
void collect(const Statement& statement, std::vector<std::shared_ptr<Expr>>& out) {
    findSubqueries(statement.where, out);
    findSubqueries(statement.having, out);
    for (const auto& item : statement.selectItems) findSubqueries(item.expression, out);
    for (const auto& item : statement.orderBy) findSubqueries(item.expression, out);
    for (const auto& join : statement.joins) findSubqueries(join.on, out);
    for (const auto& row : statement.valueRows) for (const auto& value : row) findSubqueries(value, out);
    for (const auto& item : statement.assignments) findSubqueries(item.expression, out);
}
}

int main() {
    const char* sql0 = "SELECT * FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.id=t.id);";
    const char* sql1 = "SELECT * FROM t WHERE id NOT IN (SELECT id FROM u);";
    const char* sql2 = "SELECT (SELECT MAX(v) FROM u) AS top FROM t;";
    const auto statements = parse(tokenize(std::string(sql0) + sql1 + sql2));
    require(statements.size() == 3, "expected three statements");

    std::size_t count = 0;
    for (const auto& statement : statements) {
        std::vector<std::shared_ptr<Expr>> subqueries;
        collect(statement, subqueries);
        for (const auto& node : subqueries) {
            require(node->subquery != nullptr, "missing structured subquery node");
            require(node->subquery->kind == "Select", "subquery node is not a Select");
            require(!node->subquery->table.empty(), "subquery node must carry its FROM table");
            require(!node->subquerySql.empty(), "interim subquerySql must remain populated");
            ++count;
        }
    }
    require(count == 3, "expected EXISTS/IN/scalar subquery nodes");
    std::cout << "3 X09 structured-subquery parser checks passed\n";

    // --- X09 Phase 3.2: FROM derived tables ---
    auto single = [](const std::string& sql) { return parse(tokenize(sql)).front(); };
    const auto derived = single("SELECT d.x FROM (SELECT a AS x FROM t) AS d;");
    require(derived.fromSubquery != nullptr, "derived table must carry a structured subquery");
    require(derived.fromSubquery->kind == "Select", "derived table subquery is not a Select");
    require(derived.tableAlias == "d", "derived table explicit alias not captured");
    require(derived.fromSubquery->selectItems.size() == 1, "derived subquery must keep its select items");

    auto rejected = [&](const std::string& sql) {
        try { (void)single(sql); return false; } catch (const minisql::MiniSqlError&) { return true; }
    };
    require(rejected("SELECT * FROM (SELECT a FROM t);"), "derived table without explicit alias must be rejected");
    require(rejected("SELECT * FROM (SELECT a AS x, b AS x FROM t) AS d;"), "duplicate derived output column names must be rejected");
    std::cout << "4 X09 derived-table parser checks passed\n";

    // --- X09 Phase 3.3: derived-table scope-chain planning (compilePlans) ---
    minisql::catalog::Catalog catalog;
    for (const auto& s : parse(tokenize("CREATE TABLE t(id INT, a INT, b INT); CREATE TABLE s(id INT, v INT);")))
        minisql::catalog::validate({s}, catalog);
    auto parseOne = [](const std::string& sql) { return parse(tokenize(sql)).front(); };

    const auto derivedPlan = compilePlans({parseOne("SELECT d.x, d.y FROM (SELECT a AS x, b AS y FROM t WHERE a>=20) AS d;")}, catalog).front();
    require(derivedPlan.kind == "Project", "derived outer plan root is a Project");
    require(derivedPlan.table == "d", "derived plan is named by alias");
    require(derivedPlan.output.size() == 2, "derived outer projection exposes two output columns");
    require(derivedPlan.output[0].name == "x" && derivedPlan.output[1].name == "y", "derived outer output uses alias names");
    // 外层 Project 的孩子应为满足派生关系的子计划（Filter→Project 或 Project）。
    require(!derivedPlan.children.empty(), "derived outer plan has a base subplan");
    // 外层投影通过列引用读取派生输出列。
    for (const auto& projection : derivedPlan.projections)
        require(projection.value("kind", "") == "Identifier", "derived outer projection is bound to a derived column");

    bool derivedMissingColumn = false;
    try {
        (void)compilePlans({parseOne("SELECT nope FROM (SELECT a AS x FROM t) AS d;")}, catalog);
    } catch (const minisql::MiniSqlError&) { derivedMissingColumn = true; }
    require(derivedMissingColumn, "unknown derived outer column must be rejected");
    std::cout << "3 X09 derived-table planner checks passed\n";
    return 0;
}