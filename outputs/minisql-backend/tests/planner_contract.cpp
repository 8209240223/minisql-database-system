#include "minisql/sql/planner.hpp"
#include "minisql/sql/serialization.hpp"
#include "minisql/sql/lr_generator.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

void require(bool condition) {
    if (!condition) throw std::runtime_error("Catalog snapshot contract failed");
}

int main() {
    minisql::catalog::Catalog catalog;
    auto parse = [](const std::string& sql) { return minisql::sql::parse(minisql::sql::tokenize(sql)); };
    auto existing = parse("CREATE TABLE existing(id INT);");
    minisql::catalog::validate(existing, catalog);
    auto plans = minisql::sql::compilePlans(parse("CREATE TABLE temp(id INT); SELECT id FROM temp;"), catalog);
    require(plans.size() == 2);
    require(catalog.find("temp") == nullptr);
    require(catalog.find("existing") != nullptr);
    bool rejected = false;
    try {
        (void)minisql::sql::compilePlans(parse("CREATE TABLE broken(id INT); SELECT missing FROM broken;"), catalog);
    } catch (const minisql::MiniSqlError&) {
        rejected = true;
    }
    require(rejected);
    require(catalog.find("broken") == nullptr);
    require(catalog.find("existing") != nullptr);
    auto valid = minisql::sql::compilePlans(parse("SELECT id FROM existing;"), catalog);
    require(valid.front().kind == "Project");
    auto structured = minisql::sql::compilePlans(parse("SELECT id FROM existing WHERE id>=1 ORDER BY id DESC LIMIT 2;"), catalog);
    const auto document = minisql::sql::serializePlans(structured);
    const auto restored = minisql::sql::deserializePlans(document);
    require(minisql::sql::serializePlans(restored) == document);
    const auto wrapped = nlohmann::json{{"schemaVersion", 1}, {"planKind", "logical"}, {"plans", document}};
    require(minisql::sql::serializePlans(minisql::sql::deserializePlans(wrapped)) == document);
    bool badVersion = false;
    try { (void)minisql::sql::deserializePlans(nlohmann::json{{"schemaVersion", 2}, {"planKind", "logical"}, {"plans", document}}); }
    catch (const minisql::MiniSqlError&) { badVersion = true; }
    require(badVersion);
    auto damagedParent = document;
    damagedParent[0]["children"] = nlohmann::json::array({99});
    bool badChild = false;
    try { (void)minisql::sql::deserializePlans(damagedParent); }
    catch (const minisql::MiniSqlError&) { badChild = true; }
    require(badChild);
    auto damagedId = document;
    damagedId[0]["id"] = 7;
    bool badId = false;
    try { (void)minisql::sql::deserializePlans(damagedId); }
    catch (const minisql::MiniSqlError&) { badId = true; }
    require(badId);
    auto damagedExpression = document;
    for (auto& node : damagedExpression) if (node.contains("predicate") && !node["predicate"].is_null()) node["predicate"] = nlohmann::json{{"kind", "Unknown"}};
    bool badExpression = false;
    try { (void)minisql::sql::deserializePlans(damagedExpression); }
    catch (const minisql::MiniSqlError&) { badExpression = true; }
    require(badExpression);
    const auto ast = parse("CREATE TABLE ast_t(id INT PRIMARY KEY,v FLOAT,s VARCHAR(20),CHECK(v IS NULL OR v>0)); INSERT INTO ast_t VALUES(1,1.5,'x'),(2,NULL,'y'); SELECT id,v FROM ast_t WHERE id IN (SELECT id FROM ast_t) ORDER BY id DESC LIMIT 1; UPDATE ast_t SET v=CAST(2 AS FLOAT) WHERE id=1; DELETE FROM ast_t WHERE id=2;");
    const auto astDocument = minisql::sql::serializeAst(ast);
    const auto astRestored = minisql::sql::deserializeAst(astDocument);
    require(minisql::sql::serializeAst(astRestored) == astDocument);
    const auto astWrapped = nlohmann::json{{"schemaVersion", 1}, {"statements", astDocument}};
    require(minisql::sql::serializeAst(minisql::sql::deserializeAst(astWrapped)) == astDocument);
    bool badAstVersion = false;
    try { (void)minisql::sql::deserializeAst(nlohmann::json{{"schemaVersion", 2}, {"statements", astDocument}}); }
    catch (const minisql::MiniSqlError&) { badAstVersion = true; }
    require(badAstVersion);
    auto damagedAst = astDocument;
    damagedAst[1]["valueRows"][0][0] = nlohmann::json{{"kind", "Unknown"}};
    bool badAstExpression = false;
    try { (void)minisql::sql::deserializeAst(damagedAst); }
    catch (const minisql::MiniSqlError&) { badAstExpression = true; }
    require(badAstExpression);
    const auto lr = minisql::sql::buildCanonicalLr0({
        {"S", {"A"}},
        {"A", {"a"}},
    });
    require(lr.productions.size() == 3);
    require(lr.states.size() >= 2);
    const auto accept = std::find_if(lr.actions.begin(), lr.actions.end(), [](const auto& entry) { return entry.second == "accept"; });
    require(accept != lr.actions.end());
    const auto lalr = minisql::sql::buildLalr({
        {"S", {"A"}},
        {"A", {"a"}},
    });
    require(!lalr.states.empty());
    const auto lalrAccept = std::find_if(lalr.actions.begin(), lalr.actions.end(), [](const auto& entry) { return entry.second == "accept"; });
    require(lalrAccept != lalr.actions.end());
    std::cout << "Catalog snapshot, plan JSON and AST JSON contract checks passed\n";
}
