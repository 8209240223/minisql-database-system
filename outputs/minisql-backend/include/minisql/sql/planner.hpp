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
// 计划里描述"输出列"的结构，决定结果集元数据与列绑定。
    std::string name;
    // 列名。
    std::string type;
    // 列类型（如 int、varchar(20)、decimal(10,2)）。
    std::size_t columnId;
    // 该列在本表内的物理列序号，执行层按它取值。
    bool nullable = true;
    // 该列是否允许为 NULL。
    std::optional<std::string> defaultValue{};
    // 默认值原文，没有默认值则为空。
    bool primaryKey = false;
    // 是否属于主键。
    bool unique = false;
    // 是否带唯一约束。
    std::optional<std::pair<std::string,std::string>> references{};
    // 列级外键（父表名，父列名），没有则为空。
    std::uint32_t binding = 0;   // sql::ColumnId
    std::uint32_t relation = 0;  // sql::RelationId：该列来自哪个 FROM 项
    std::uint32_t expression = 0; // sql::ExpressionId for computed output columns
};

struct LogicalPlan {
// 逻辑计划节点，一个节点代表一个算子；children 里挂它的输入算子。
    std::string kind;
    // 算子种类，如 TableScan / Filter / Project / Join / Aggregate / Sort / Limit。
    std::string table;
    // 该算子访问的表名（表扫描算子使用）。
    std::vector<PlanColumn> output;
    // 该算子的输出列定义。
    std::vector<LogicalPlan> children;
    // 子算子列表，按数据流入顺序排列。
    nlohmann::json predicate;
    // 过滤谓词（WHERE / HAVING / JOIN ON 的表达式），用 JSON 表示表达式树。
    nlohmann::json values = nlohmann::json::array();
    // 直接写入的值列表（INSERT 的字面量形式）。
    std::vector<std::size_t> columnMapping;
    // 输出列到输入列的映射，投影算子用它实现"重新排布列"。
    bool preservesRowId = false;
    // 该算子是否保持输入行的物理行号不变，影响上层能否直接按行号回表。
    nlohmann::json projections = nlohmann::json::array();
    // 投影表达式列表。
    std::optional<std::uint64_t> limit{};
    // LIMIT 条数，未设置时为空。
    std::uint64_t offset = 0;
    // OFFSET 偏移量。
    nlohmann::json sortKeys = nlohmann::json::array();
    // 排序键列表，每项包含表达式与升降序信息。
    nlohmann::json insertExpressions = nlohmann::json::array();
    // INSERT 的表达式形式值列表（支持 DEFAULT 与表达式）。
    nlohmann::json insertRows = nlohmann::json::array();
    // INSERT 多行形式：每个元素是一行的表达式序列。
    std::vector<KeyConstraint> keys{};
    // 建表计划要落库的主键/唯一键约束。
    nlohmann::json checks = nlohmann::json::array();
    // CHECK 约束表达式的序列化内容。
    nlohmann::json checkDefinitions = nlohmann::json::array();
    // CHECK 约束的元数据（约束名与目标绑定）。
    std::vector<ForeignKey> foreignKeys{};
    // 建表计划要落库的外键列表。
    std::vector<ConstraintName> constraintNames{};
    // 约束名绑定列表，随建表计划一起持久化。
    nlohmann::json groupKeys = nlohmann::json::array();
    // GROUP BY 的分组键表达式列表。
    nlohmann::json aggregates = nlohmann::json::array();
    // 聚合函数调用列表（COUNT/SUM/AVG/MIN/MAX 及其参数）。
    std::string indexName{};
    // 索引相关算子使用的索引名。
    std::string savepointName{};
    // 事务算子使用的保存点名。
    std::string subqueryJoinKind{};
    // 子查询连接的连接类型（如 semi、anti），用于半连接/反连接改写。
    bool uniqueIndex = false;
    // 建索引时是否要求唯一。
    std::vector<std::string> indexColumns{};
    // 索引列名列表。
    nlohmann::json indexValues = nlohmann::json::array();
    // 走索引时要匹配的值序列。
    std::string indexRangeOperator{};
    // 索引范围扫描的比较运算符（如 >、>=、<、<=），空表示等值或整表扫描。
    nlohmann::json indexRangeValue = nullptr;
    // 索引范围扫描的边界值，与 indexRangeOperator 配对使用。
    // 编译期 Catalog 指纹；执行时与当前 Catalog 不一致即 PLAN_STALE_SCHEMA。
    std::string catalogFingerprint{};
    SourceLocation sourceSpan{};
    // Optimizer candidate costs and deterministic choice, exposed by plan JSON/EXPLAIN.
    nlohmann::json optimizerDecision = nullptr;
};

std::vector<LogicalPlan> compilePlans(const std::vector<Statement>& statements,
                                      const catalog::Catalog& catalog);
// 编译主入口：把语法树语句列表翻译成逻辑计划列表，期间用目录做语义校验与类型推导。
nlohmann::json serializePlans(const std::vector<LogicalPlan>& plans);
// 把计划列表序列化成 JSON，便于通过进程间通道或文件传给执行层。
std::vector<LogicalPlan> deserializePlans(const nlohmann::json& document);
// 反序列化：把 JSON 文本还原成计划列表，执行层据此跑算子。
}
