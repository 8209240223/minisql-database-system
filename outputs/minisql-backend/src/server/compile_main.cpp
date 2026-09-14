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
// 简写 JSON 命名空间。
namespace {
// 从 SQL 文件读取源码：显式失败优于静默空输入，并去掉 UTF-8 BOM。
// （承接上一行）"显式失败"指打不开文件就直接报错，而不是当成空脚本继续跑。
std::string displayPath(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
// 从 SQL 文件读取源码：显式失败优于静默空输入，并去掉 UTF-8 BOM。
std::string readSqlFile(const std::filesystem::path& path) {
// 读取 SQL 文件内容。
    std::ifstream stream(path, std::ios::binary);
    // 以二进制方式打开，避免平台做换行转换。
    if (!stream) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot open SQL file: " + displayPath(path));
    std::string source{std::istreambuf_iterator<char>(stream), {}};
    // 一次性把文件内容读进字符串。
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        // 检查是不是 UTF-8 BOM（EF BB BF）的开头两个字节。
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF)
        // 检查 BOM 的第三个字节。
        source.erase(0, 3);
        // 去掉 BOM，否则第一个 token 会带上看不见的字符导致解析失败。
    return source;
    // 返回纯文本源码。
}
// 读取函数结束。
}
int main(int argc, char** argv) {
// 编译前端的独立入口：读 SQL，依次跑词法、语法、语义、计划、优化，最后输出 JSON。
    try {
    // 所有错误都以异常形式抛出，这里统一捕获。
        bool parseOnly = false;
        // 是否只做解析而不生成计划（--parse-only）。
        std::filesystem::path sqlFile;
        // --file 指定的 SQL 文件；为空表示从标准输入读。
        int sqlFileArgIndex = -1;
        for (int index = 1; index < argc; ++index) {
        // 手写一个很小的参数循环：选项少，不必引入解析库。
            const std::string argument = argv[index];
            // 当前参数。
            if (argument == "--parse-only") { parseOnly = true; continue; }
            // 只解析模式：跳过语义与计划阶段。
            if (argument == "--file" || argument == "-f") {
            // 指定 SQL 文件。
                if (index + 1 >= argc) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "--file requires a path");
                // 选项后面没跟路径，属于参数错误。
                sqlFileArgIndex = ++index;
                sqlFile = argv[index];
                continue;
                // 处理完继续下一个参数。
            }
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown option: " + argument);
            // 其余参数一律拒绝，避免拼错选项却静默跑错。
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
        // 没给文件就读标准输入，否则读文件，得到统一的源码字符串。
        auto tok = minisql::sql::tokenize(source);
        // 词法分析：源码 → token 流。
        auto ast = minisql::sql::parse(tok);
        // 语法分析：token 流 → 语法树。
        const auto tokens = minisql::sql::serializeTokens(tok);
        // 把 token 流序列化成 JSON，作为输出的一部分。
        const auto nodes = minisql::sql::serializeAst(ast);
        // 把语法树序列化成 JSON。
        json plans = json::array();
        // 逻辑计划占位；--parse-only 时保持空数组。
        json optimizedPlans = json::array(), rules = json::array();
        // 优化后的计划与命中的优化规则，同样先占位。
        json optimizerStatus = {{"iterations", 0}, {"converged", false}, {"diagnostics", json::array()}, {"rules", minisql::optimizer::ruleDescriptors()}};
        // 优化器状态外壳：迭代轮数、是否收敛、诊断信息，以及全部可用规则的描述。
        if (!parseOnly) {
        // 非只解析模式才继续往下走。
            const minisql::catalog::Catalog catalog;
            // 用一份空目录做编译：这里只验证语法与计划生成，不连接真实数据。
            const auto original = minisql::sql::compilePlans(ast, catalog);
            // 语义分析 + 计划生成，得到优化前的逻辑计划。
            plans = minisql::sql::serializePlans(original);
            // 序列化原计划。
            // plan[*].checkDefinitions 必须与上面 ast 里的 checks 逐节点相等
            // （tests/check-process.mjs 的 checkDefinitions == ast.checks 断言依赖这一点）。
            // ast 侧由 serializeAst 内的 annotateAst 分配 nodeId/sourceSpan，
            // 而 checkDefinitions 由 serializePlans 单独序列化、拿不到同一套编号。
            // 两者本是同一批表达式树的副本，这里用已标注的 ast 侧覆盖计划侧，保证一致。
            // serializeAst 对单条语句返回裸对象、多条语句才返回数组，这里统一成数组再逐条对齐。
            const auto astStatements = nodes.is_array() ? nodes : json::array({nodes});
            for (std::size_t i = 0; i < astStatements.size() && i < plans.size(); ++i) {
                if (astStatements[i].is_object() && astStatements[i].contains("checks") && plans[i].is_object())
                    plans[i]["checkDefinitions"] = astStatements[i]["checks"];
            }
            const auto optimized = minisql::optimizer::optimize(original);
            // 跑优化器，得到改写后的计划以及改写记录。
            optimizedPlans = minisql::sql::serializePlans(optimized.plans);
            // 序列化优化后的计划。
            // 优化后的计划同样要与 ast 的 checks 保持一致（见上面原计划的说明）。
            for (std::size_t i = 0; i < astStatements.size() && i < optimizedPlans.size(); ++i) {
                if (astStatements[i].is_object() && astStatements[i].contains("checks") && optimizedPlans[i].is_object())
                    optimizedPlans[i]["checkDefinitions"] = astStatements[i]["checks"];
            }
            rules = optimized.changes;
            // 记录每一处改写，便于在界面上展示优化过程。
            optimizerStatus["iterations"] = optimized.iterations;
            // 回填实际迭代轮数。
            optimizerStatus["converged"] = optimized.converged;
            // 回填是否收敛。
            optimizerStatus["diagnostics"] = optimized.diagnostics;
            // 回填优化器诊断信息。
        }
        std::cout << minisql::wireJson(json{{"success", true}, {"tokens", tokens}, {"ast", nodes}, {"integerEncoding", "safe-number-or-decimal-string"},
                           // 输出统一结果外壳；integerEncoding 说明大整数是按安全范围还是十进制字符串编码。
                          {"plan", plans}, {"schemaVersion", 1}, {"planKind", "logical"}, {"rows", json::array()}, {"columns", json::array()},
                           // 计划、版本号、计划种类；这里不执行查询，所以行与列都是空数组。
                          {"affectedRows", 0}, {"statements", ast.size()}, {"optimizedPlan", optimizedPlans}, {"optimizationRules", rules},
                           // 影响行数、语句条数、优化后计划与命中规则。
                          {"optimizer", optimizerStatus},
                           // 优化器状态。
                          {"stages", {{"lexer", "passed"}, {"parser", "passed"},
                                       // 词法与语法阶段一定已经跑过。
                                      {"semantic", parseOnly ? "skipped" : "passed"}, {"planner", parseOnly ? "skipped" : "passed"},
                                       // 只解析模式下语义与计划阶段标记为 skipped。
                                      {"optimizer", parseOnly ? "skipped" : "passed"}, {"executor", "notImplemented"}}}}).dump();
                                       // 优化器同理；执行阶段当前固定为 notImplemented。
        return 0;
        // 正常结束。
    } catch (const minisql::MiniSqlError& e) {
    // 捕获编译期错误并输出其 JSON 表示。
        std::cout << e.toJson().dump();
        // 直接写标准输出：调用方无论成功失败都按同一份 JSON 协议解析。
        return 1;
        // 失败退出码。
    }
}
// 入口函数结束。
