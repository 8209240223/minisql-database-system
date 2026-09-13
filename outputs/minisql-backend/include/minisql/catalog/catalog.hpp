#pragma once
#include "minisql/sql/parser.hpp"
#include <unordered_map>
namespace minisql::catalog {
struct Column { std::string name; std::string type; std::string qualifier{}; bool nullable = true; std::optional<std::string> defaultValue{}; bool primaryKey = false; bool unique = false; std::optional<std::pair<std::string,std::string>> references{}; };
struct Index { std::string name; std::vector<std::string> columns; bool unique = false; };
struct Table { std::string name; std::vector<Column> columns; std::vector<sql::KeyConstraint> keys{}; std::vector<std::string> checks{}; std::vector<sql::ForeignKey> foreignKeys{}; std::vector<sql::ConstraintName> constraintNames{}; std::vector<Index> indexes{}; };
class Catalog {
public:
    void create(const sql::Statement& statement);
    void createIndex(const sql::Statement& statement);
    void dropIndex(const sql::Statement& statement);
    const Table* find(const std::string& name) const;
    const Table* findIndexTable(const std::string& indexName) const;
    // 稳定 schema 指纹：表名 + 列名/类型/可空/主键/唯一，按表名排序后哈希。
    // 计划绑定该指纹；执行时不一致即 PLAN_STALE_SCHEMA（第十七章 REQ-CORE-002）。
    std::string schemaFingerprint() const;
private:
    std::unordered_map<std::string, Table> tables_;
};
void validate(const std::vector<sql::Statement>& statements, Catalog& catalog);
std::vector<std::string> insertColumns(const sql::Statement& statement, const Table& table);
Catalog compileSnapshot(const std::vector<sql::Statement>& statements, const Catalog& catalog);
std::string resolveColumnName(const Table& table, const std::string& name, SourceLocation location = {});
std::size_t resolveColumnIndex(const Table& table, const std::string& name, SourceLocation location = {});
std::string notNullConstraintSuffix(const Table& table, std::size_t index);
Table queryScope(const sql::Statement& statement, const Catalog& catalog, std::size_t joinCount = static_cast<std::size_t>(-1));
std::shared_ptr<sql::Expr> resolveOrder(const sql::Statement& statement, const sql::OrderItem& item, const Table& table);
struct SelectAnalysis {
    bool aggregated = false;
    std::vector<std::string> projectionTypes;
    std::vector<std::string> groupTypes;
};
SelectAnalysis analyzeSelect(const sql::Statement& statement, const Table& scope);
}
