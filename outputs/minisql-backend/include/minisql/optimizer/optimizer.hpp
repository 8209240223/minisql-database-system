#pragma once
#include "minisql/sql/planner.hpp"
#include <unordered_map>

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
    bool decorrelateSubquery = true;
    double defaultTableRows = 1000.0;
    std::size_t memoryBudgetBytes = 64 * 1024 * 1024;
    std::unordered_map<std::string, double> tableRows{};
    std::size_t maxIterations = 16;
    std::size_t maxNodes = 65536;
    std::vector<std::string> disabledRules{};
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
