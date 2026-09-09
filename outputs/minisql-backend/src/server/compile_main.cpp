#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"
#include "minisql/catalog/catalog.hpp"
#include "minisql/sql/planner.hpp"
#include "minisql/optimizer/optimizer.hpp"
#include "minisql/sql/serialization.hpp"
#include "minisql/common/wire_json.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <iterator>
using json = nlohmann::json;
int main(int argc, char** argv) {
    try {
        const bool parseOnly = argc == 2 && std::string(argv[1]) == "--parse-only";
        if (argc > 1 && !parseOnly) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown option");
        std::string source{std::istreambuf_iterator<char>(std::cin), {}};
        auto tok = minisql::sql::tokenize(source);
        auto ast = minisql::sql::parse(tok);
        const auto tokens = minisql::sql::serializeTokens(tok);
        const auto nodes = minisql::sql::serializeAst(ast);
        json plans = json::array();
        json optimizedPlans = json::array(), rules = json::array();
        json optimizerStatus = {{"iterations", 0}, {"converged", false}, {"diagnostics", json::array()}, {"rules", minisql::optimizer::ruleDescriptors()}};
        if (!parseOnly) {
            const minisql::catalog::Catalog catalog;
            const auto original = minisql::sql::compilePlans(ast, catalog);
            plans = minisql::sql::serializePlans(original);
            const auto optimized = minisql::optimizer::optimize(original);
            optimizedPlans = minisql::sql::serializePlans(optimized.plans);
            rules = optimized.changes;
            optimizerStatus["iterations"] = optimized.iterations;
            optimizerStatus["converged"] = optimized.converged;
            optimizerStatus["diagnostics"] = optimized.diagnostics;
        }
        std::cout << minisql::wireJson(json{{"success", true}, {"tokens", tokens}, {"ast", nodes}, {"integerEncoding", "safe-number-or-decimal-string"},
                          {"plan", plans}, {"schemaVersion", 1}, {"planKind", "logical"}, {"rows", json::array()}, {"columns", json::array()},
                          {"affectedRows", 0}, {"statements", ast.size()}, {"optimizedPlan", optimizedPlans}, {"optimizationRules", rules},
                          {"optimizer", optimizerStatus},
                          {"stages", {{"lexer", "passed"}, {"parser", "passed"},
                                      {"semantic", parseOnly ? "skipped" : "passed"}, {"planner", parseOnly ? "skipped" : "passed"},
                                      {"optimizer", parseOnly ? "skipped" : "passed"}, {"executor", "notImplemented"}}}}).dump();
        return 0;
    } catch (const minisql::MiniSqlError& e) {
        std::cout << e.toJson().dump();
        return 1;
    }
}
