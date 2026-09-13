#pragma once
#include "minisql/catalog/catalog.hpp"
#include "minisql/sql/binding.hpp"
#include <nlohmann/json.hpp>

namespace minisql::sql {
// X25：columnId 与 binding 是两件事，此前被同一个字段兼任。
//   columnId —— 运行时槽位：执行器用它索引子节点产出的行（row.at(columnId)）。
//               JOIN 会把左右两侧的槽位拼接，谓词下推因此需要重基。
//   binding  —— 稳定身份（sql::ColumnId 的原始值，0 表示没有绑定身份，
//               例如表达式列、聚合输出、排序临时列）。裁剪、下推、计划对比
//               都应当看它，因为它不随槽位变化。
struct PlanColumn {
    std::string name;
    std::string type;
    std::size_t columnId;
    bool nullable = true;
    std::optional<std::string> defaultValue{};
    bool primaryKey = false;
    bool unique = false;
    std::optional<std::pair<std::string,std::string>> references{};
    std::uint32_t binding = 0;   // sql::ColumnId
    std::uint32_t relation = 0;  // sql::RelationId：该列来自哪个 FROM 项
    std::uint32_t expression = 0; // sql::ExpressionId for computed output columns
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
    // 编译期 Catalog 指纹；执行时与当前 Catalog 不一致即 PLAN_STALE_SCHEMA。
    std::string catalogFingerprint{};
    SourceLocation sourceSpan{};
    // Optimizer candidate costs and deterministic choice, exposed by plan JSON/EXPLAIN.
    nlohmann::json optimizerDecision = nullptr;
};

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
                                      const catalog::Catalog& catalog);
nlohmann::json serializePlans(const std::vector<LogicalPlan>& plans);
std::vector<LogicalPlan> deserializePlans(const nlohmann::json& document);
}
