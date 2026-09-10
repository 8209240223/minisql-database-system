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
