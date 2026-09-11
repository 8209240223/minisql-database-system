#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"
#include "minisql/catalog/catalog.hpp"
#include "minisql/sql/planner.hpp"
#include "minisql/optimizer/optimizer.hpp"
#include "minisql/sql/serialization.hpp"
#include "minisql/common/wire_json.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
using json = nlohmann::json;
namespace {
std::string displayPath(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
// 从 SQL 文件读取源码：显式失败优于静默空输入，并去掉 UTF-8 BOM。
std::string readSqlFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot open SQL file: " + displayPath(path));
    std::string source{std::istreambuf_iterator<char>(stream), {}};
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF)
        source.erase(0, 3);
    return source;
}
}
int main(int argc, char** argv) {
    try {
        bool parseOnly = false;
        std::filesystem::path sqlFile;
        int sqlFileArgIndex = -1;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--parse-only") { parseOnly = true; continue; }
            if (argument == "--file" || argument == "-f") {
                if (index + 1 >= argc) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "--file requires a path");
                sqlFileArgIndex = ++index;
                sqlFile = argv[index];
                continue;
            }
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown option: " + argument);
        }
#ifdef _WIN32
        int wideCount = 0;
        auto wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (!wideArgs || wideCount != argc) {
            if (wideArgs) LocalFree(wideArgs);
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot decode command line");
        }
        if (sqlFileArgIndex >= 0) sqlFile = wideArgs[sqlFileArgIndex];
        LocalFree(wideArgs);
#endif
        const std::string source = sqlFile.empty() ? std::string{std::istreambuf_iterator<char>(std::cin), {}} : readSqlFile(sqlFile);
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
