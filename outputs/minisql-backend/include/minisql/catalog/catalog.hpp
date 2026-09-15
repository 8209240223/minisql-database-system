#pragma once
#include "minisql/sql/parser.hpp"
#include <unordered_map>
namespace minisql::catalog {
struct Column { std::string name; std::string type; std::string qualifier{}; bool nullable = true; std::optional<std::string> defaultValue{}; bool primaryKey = false; bool unique = false; std::optional<std::pair<std::string,std::string>> references{}; };
// 列定义：名字、类型、所属表限定名、是否可空、默认值、主键、唯一、外键引用目标。
struct Index { std::string name; std::vector<std::string> columns; bool unique = false; };
// 索引定义：名字、组成列与是否唯一。
struct Table { std::string name; std::vector<Column> columns; std::vector<sql::KeyConstraint> keys{}; std::vector<std::string> checks{}; std::vector<sql::ForeignKey> foreignKeys{}; std::vector<sql::ConstraintName> constraintNames{}; std::vector<Index> indexes{}; };
// 表定义：名字、列、键约束、CHECK 表达式、外键、约束命名与索引。
class Catalog {
// 目录：内存中的表定义集合，相当于编译原理里的符号表。
public:
    void create(const sql::Statement& statement);
    // 建表并做全部列级校验。
    void createIndex(const sql::Statement& statement);
    // 建索引并登记到对应表上。
    void dropIndex(const sql::Statement& statement);
    // 删除索引。
    const Table* find(const std::string& name) const;
    // 按表名查找；找不到返回空指针（名字比较不区分大小写）。
    const Table* findIndexTable(const std::string& indexName) const;
    // 稳定 schema 指纹：表名 + 列名/类型/可空/主键/唯一，按表名排序后哈希。
    // 计划绑定该指纹；执行时不一致即 PLAN_STALE_SCHEMA（第十七章 REQ-CORE-002）。
    std::string schemaFingerprint() const;
private:
    std::unordered_map<std::string, Table> tables_;
    // 表名（已转小写）到表定义的映射。
};
void validate(const std::vector<sql::Statement>& statements, Catalog& catalog);
// 对一批语句做语义校验，建表与建索引会顺带更新目录。
std::vector<std::string> insertColumns(const sql::Statement& statement, const Table& table);
// 推导 INSERT 实际写入哪些列：显式列名优先，否则按表定义顺序全列。
Catalog compileSnapshot(const std::vector<sql::Statement>& statements, const Catalog& catalog);
// 在目录副本上做校验，得到“这几条语句执行完之后”的目录快照，不改动原目录。
std::string resolveColumnName(const Table& table, const std::string& name, SourceLocation location = {});
// 去掉限定名前缀，得到纯列名；限定名不存在时报错。
std::size_t resolveColumnIndex(const Table& table, const std::string& name, SourceLocation location = {});
// 把列名解析成列下标；列不存在或名字有歧义时报错。
std::string notNullConstraintSuffix(const Table& table, std::size_t index);
// 为 NOT NULL 报错找出约束名，便于提示是哪条约束被违反。
Table derivedTableScope(const sql::Statement& inner, const Catalog& catalog, std::size_t depth = 0);
// 把一个派生表的内层 SELECT 算成一张“虚拟表”（列名 + 类型）。
// 派生表不在 catalog 里，它的输出列必须由调用方预先算出来，
// planner 依靠它把派生表参与到作用域与类型校验里。
Table queryScope(const sql::Statement& statement, const Catalog& catalog, std::size_t joinCount = static_cast<std::size_t>(-1),
                  const std::unordered_map<std::string, Table>* derivedScopes = nullptr);
// derivedScopes：JOIN 右侧是派生表时用的作用域表（按别名索引）。
// 派生表不在 catalog 里，它的输出列只有 planer 能算出来，
// 所以由调用方预先算好并传入；为空表示没有派生表参与连接。
// 构造查询作用域：主表加参与连接的表的列合并成一张“视角表”，供名字解析使用。
std::shared_ptr<sql::Expr> resolveOrder(const sql::Statement& statement, const sql::OrderItem& item, const Table& table);
// 解析 ORDER BY：优先匹配投影别名，其次按普通列处理。
struct SelectAnalysis {
// SELECT 的语义分析结果。
    bool aggregated = false;
    // 是否属于聚合查询（含 GROUP BY、HAVING 或聚合函数）。
    std::vector<std::string> projectionTypes;
    // 每一投影列的类型。
    std::vector<std::string> groupTypes;
    // 每个分组键的类型。
};
SelectAnalysis analyzeSelect(const sql::Statement& statement, const Table& scope);
// 分析 SELECT：判定是否聚合、校验投影与 ORDER BY、检查“必须分组或聚合”。
}
