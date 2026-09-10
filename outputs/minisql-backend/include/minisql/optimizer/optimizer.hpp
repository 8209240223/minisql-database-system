#pragma once
#include "minisql/sql/planner.hpp"
#include <functional>
#include <optional>

namespace minisql::optimizer {
struct Options {
    bool constantComparison = true;
    bool booleanSimplification = true;
    bool removeTrueFilter = true;
    bool removeFalseFilter = true;
    bool predicatePushdown = true;
    bool hashJoin = true;
    bool pruneColumns = true;
    bool constantArithmetic = true;
    std::size_t maxIterations = 16;
    std::size_t maxNodes = 65536;
    std::vector<std::string> disabledRules{};
    // X18 4.2: 可选的表行数回调（真实统计）。仅当提供时，SeqScan 行数估算用它，
    // 否则回退到有界默认值 kOptimizerDefaultRows；同一次 optimize 内确定。
    std::function<std::optional<double>(const std::string&)> tableRows{};
    // X18 4.2-iv: 可选的列级选择率回调（直方图驱动）。Filter 行数估算用它对谓词做
    // 列级选择率（含 AND/OR/NOT 连乘与交），而非统一默认 0.25，使旁路下推后的过滤
    // 选择率真实影响 join 的 Hash/NL 成本选择；未提供则回退 kOptimizerDefaultFilterSelectivity。
    // 签名同 database.cpp 的 columnSelectivity：(predicate, tableName) -> selectivity in [0,1]。
    std::function<double(const nlohmann::json&, const std::string&)> selectivity{};
};
struct Result {
    std::vector<sql::LogicalPlan> plans;
    nlohmann::json changes = nlohmann::json::array();
    std::size_t iterations = 0;
    bool converged = false;
    nlohmann::json diagnostics = nlohmann::json::array();
};
nlohmann::json ruleDescriptors();
Result optimize(const std::vector<sql::LogicalPlan>& plans, Options options = {});
}
