#pragma once
#include "minisql/catalog/catalog.hpp"
#include <nlohmann/json.hpp>

namespace minisql::sql {
struct PlanColumn {
    std::string name;
    std::string type;
    std::size_t columnId;
    bool nullable = true;
    std::optional<std::string> defaultValue{};
    bool primaryKey = false;
    bool unique = false;
    std::optional<std::pair<std::string,std::string>> references{};
    std::string identity{};
};

struct LogicalPlan {
    std::string kind;
    std::string table;
    std::vector<PlanColumn> output;
    std::vector<LogicalPlan> children;
    nlohmann::json predicate;
    nlohmann::json values = nlohmann::json::array();
    std::vector<std::size_t> columnMapping;
    bool preservesRowId = false;
    nlohmann::json projections = nlohmann::json::array();
    std::optional<std::uint64_t> limit{};
    std::uint64_t offset = 0;
    nlohmann::json sortKeys = nlohmann::json::array();
    nlohmann::json insertExpressions = nlohmann::json::array();
    nlohmann::json insertRows = nlohmann::json::array();
    std::vector<KeyConstraint> keys{};
    nlohmann::json checks = nlohmann::json::array();
    nlohmann::json checkDefinitions = nlohmann::json::array();
    std::vector<ForeignKey> foreignKeys{};
    std::vector<ConstraintName> constraintNames{};
    nlohmann::json groupKeys = nlohmann::json::array();
    nlohmann::json aggregates = nlohmann::json::array();
    std::string indexName{};
    std::string savepointName{};
    std::string subqueryJoinKind{};
    bool uniqueIndex = false;
    std::vector<std::string> indexColumns{};
    nlohmann::json indexValues = nlohmann::json::array();
    std::string indexRangeOperator{};
    nlohmann::json indexRangeValue = nullptr;
};

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
                                      const catalog::Catalog& catalog);
nlohmann::json serializePlans(const std::vector<LogicalPlan>& plans);
std::vector<LogicalPlan> deserializePlans(const nlohmann::json& document);
}
