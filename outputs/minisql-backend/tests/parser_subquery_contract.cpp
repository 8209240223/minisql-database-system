// X09 Phase 3.1 contract: every parsed subquery expression (EXISTS / IN / scalar)
// must carry a structured statement node (Expr::subquery) alongside the interim
// SQL text (Expr::subquerySql). This verifies migration of object identity
// is in place before the planner/optimizer switch over in 3.3-3.5.
#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"
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
    return 0;
}