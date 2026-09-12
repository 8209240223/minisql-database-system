#pragma once
// X25：稳定绑定标识与作用域树。
//
// 目标：名称解析只发生一次，发生在这里。其后的优化器、序列化、权限检查都消费
// 同一份 BindResult，不再各自重新解释 SQL 文本、表名字符串或列名。
//
// 三类标识在一次绑定内稳定且稠密：
//   RelationId   —— 一个 FROM 项（基表 / 派生表 / CTE）。别名不产生新的 RelationId
//                   之外的实体；同一物理表被引用两次会得到两个不同的 RelationId。
//   ColumnId     —— 一个关系的一列。跨优化器改写保持不变，因此列裁剪、投影下推
//                   都不需要重新基于下标平移（见 docs/x25-binding-identity-progress.md）。
//   ExpressionId —— AST 中一个表达式节点。规则改写产生的新节点继承来源
//                   ExpressionId，从而诊断、计划对比、审计可以跨阶段对齐。
//
// 标识从 1 开始；0 恒为 Invalid。
#include "minisql/catalog/catalog.hpp"
#include "minisql/security/access_catalog.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace minisql::sql {

enum class RelationId : std::uint32_t { Invalid = 0 };
enum class ColumnId : std::uint32_t { Invalid = 0 };
enum class ExpressionId : std::uint32_t { Invalid = 0 };
enum class ScopeId : std::uint32_t { Invalid = 0 };

template <typename Id>
constexpr std::uint32_t rawId(Id id) noexcept { return static_cast<std::uint32_t>(id); }
template <typename Id>
constexpr bool validId(Id id) noexcept { return rawId(id) != 0; }

enum class RelationKind {
    BaseTable,              // Catalog 中存在的物理表
    DerivedTable,           // FROM ( SELECT ... ) AS alias
    CommonTableExpression   // WITH name AS ( SELECT ... )
};

struct BoundColumn {
    ColumnId id{};
    RelationId relation{};
    std::size_t ordinal = 0;  // 关系内序号，跨优化器改写稳定
    std::string name;
    std::string type;
    bool nullable = true;
};

struct BoundRelation {
    RelationId id{};
    RelationKind kind = RelationKind::BaseTable;
    std::string name;          // SQL 中书写的名字
    std::string alias;         // 作用域内可见名；无别名时等于 name
    std::string physicalName;  // 仅 BaseTable：Catalog 规范化后的物理表名（小写）
    ScopeId scope{};           // 该关系可见的作用域
    ScopeId innerScope{};      // 派生表 / CTE 的内部作用域
    SourceLocation location{};
    std::vector<ColumnId> columns;
};

// 一次列引用的解析结果。correlationDepth 为 0 表示引用当前作用域，
// >0 表示相关子查询向外层跨越的层数。
struct ColumnRef {
    ColumnId column{};
    RelationId relation{};
    std::size_t correlationDepth = 0;
};

struct Scope {
    ScopeId id{};
    ScopeId parent{};
    std::vector<RelationId> relations;  // 本层 FROM / JOIN 引入的关系
    // WITH 在本层引入的名字。它们是作用域名而非数据库对象，永远不产生受权对象。
    std::vector<RelationId> commonTableExpressions;
};

class ScopeTree {
public:
    ScopeId createScope(ScopeId parent);
    RelationId addRelation(ScopeId scope, BoundRelation relation);
    // WITH 引入的名字：登记为作用域名，不进入 FROM 关系列表，因此永不产生受权对象。
    RelationId addCommonTableExpression(ScopeId scope, BoundRelation relation);
    ColumnId addColumn(RelationId relation, BoundColumn column);

    const Scope* scope(ScopeId id) const;
    const BoundRelation* relation(RelationId id) const;
    const BoundColumn* column(ColumnId id) const;

    std::size_t scopeCount() const noexcept { return scopes_.size(); }
    std::size_t relationCount() const noexcept { return relations_.size(); }
    std::size_t columnCount() const noexcept { return columns_.size(); }

    const std::vector<Scope>& scopes() const noexcept { return scopes_; }
    const std::vector<BoundRelation>& relations() const noexcept { return relations_; }
    const std::vector<BoundColumn>& columns() const noexcept { return columns_; }

    // 在 `from` 起始，沿父链向外解析 `qualifier.name`（qualifier 可空）。
    // 找不到返回 nullopt；在同一层出现多个候选时 `ambiguous` 置为 true。
    std::optional<ColumnRef> resolveColumn(const std::string& qualifier, const std::string& name,
                                           ScopeId from, bool* ambiguous = nullptr) const;
    // 在 `from` 起始沿父链解析一个关系名（别名或 CTE 名）。
    const BoundRelation* resolveRelation(const std::string& name, ScopeId from) const;
    // candidate 是否等于 from 或是 from 的祖先作用域。
    bool atOrAbove(ScopeId candidate, ScopeId from) const;

private:
    std::vector<Scope> scopes_;            // 下标 = rawId - 1
    std::vector<BoundRelation> relations_;
    std::vector<BoundColumn> columns_;
};

struct BoundExpression {
    ExpressionId id{};
    const Expr* node = nullptr;
    ScopeId scope{};
    ExpressionId parent{};
    std::optional<ColumnRef> column;  // 仅 Identifier：解析出的列
};

struct BoundStatement {
    std::string kind;        // 与 Statement::kind 一致
    ScopeId scope{};         // 语句的顶层作用域
    RelationId target{};     // INSERT/UPDATE/DELETE/CREATE/DROP 的目标关系
    SourceLocation location{};
    std::vector<ExpressionId> expressions;  // 该语句内按前序分配的表达式
};

// 一批语句的绑定结果。complete == false 时 diagnostic 说明原因，
// 所有下游消费者（优化器、序列化、权限）都必须把它当作硬失败。
struct BindResult {
    std::vector<BoundStatement> statements;  // 前序：顶层语句与其嵌套语句交错
    std::vector<std::size_t> topLevel;       // statements 中顶层语句的下标，按书写顺序
    ScopeTree scopes;
    std::vector<BoundExpression> expressions;  // 下标 = rawId - 1
    std::unordered_map<const Expr*, ExpressionId> expressionIds;
    // Expr*/Statement* 键指向的节点必须比 BindResult 活得久。调用方传入的语句
    // 由调用方保证；绑定器自己从 Expr::subquerySql 解析出来的语句存在这里。
    std::vector<std::shared_ptr<Statement>> owned;
    std::unordered_map<const Statement*, std::size_t> statementIndex;
    // 每个子查询节点实际引用到的外层列（按 ColumnId 去重，保持首次出现顺序）。
    // 判定依据是「被引用关系所属的作用域位于该子查询所在作用域或其祖先」，
    // 不是列名文本，也不是相关深度的算术。
    std::unordered_map<const Expr*, std::vector<ColumnRef>> correlatedReferences;
    // complete 只描述「对象绑定是否闭合」：所有 FROM/JOIN/目标关系都解析到了
    // 物理表或作用域名。单个列解析失败不会清除它（那属于语义层错误），
    // 但会让对应 BoundExpression::column 保持 nullopt。
    bool complete = false;
    std::string diagnostic;
    security::AccessAction statementAction = security::AccessAction::Compile;
    std::vector<security::AccessObjectRef> objects;  // 已按 (对象, 动作) 去重

    const BoundExpression* expression(ExpressionId id) const;
    const BoundStatement* statementFor(const Statement* node) const;
    // 子查询节点引用到的外层列；不是相关子查询时返回空。
    const std::vector<ColumnRef>& correlatedFor(const Expr* subquery) const;
    ExpressionId idFor(const Expr* node) const;
    // 绑定结果到权限层的投影。complete == false 时 bound 也为 false。
    security::AccessRequest accessRequest() const;
    // 仅为兼容既有契约测试保留：按出现顺序去重的物理表名。
    std::vector<std::string> accessObjectNames() const;
};

// 绑定一批已解析语句。任何无法绑定的引用都会让结果 complete = false，
// 而不是抛出——调用方按 fail-closed 策略处理。
// 注意：BoundExpression::node 与 statementIndex 持有指向 `statements` 的裸指针，
// 调用方必须保证这批语句活得比返回的 BindResult 久。做不到就用 bindOwned。
BindResult bind(const std::vector<Statement>& statements, const catalog::Catalog& catalog);

// 同上，但取得语句所有权（存入 BindResult::owned），生命周期自洽。
BindResult bindOwned(std::vector<Statement> statements, const catalog::Catalog& catalog);

// 按指针绑定一批调用方持有的语句：BindResult 里的裸指针因此与调用方看到的
// 是同一批对象，可以用 statementFor(&statement) 反查。指针指向的语句必须
// 活得比 BindResult 久。
BindResult bindStatements(const std::vector<const Statement*>& statements, const catalog::Catalog& catalog);

// 解析 + 绑定。接受前导 EXPLAIN [ANALYZE]。永不抛出 MiniSqlError：
// 词法/语法错误体现为 complete = false 与 diagnostic。
BindResult bindSource(const std::string& source, const catalog::Catalog& catalog);

// 绑定结果的稳定序列化（诊断、计划对比、审计共用）。
nlohmann::json serializeBinding(const BindResult& result);

inline constexpr std::uint32_t BINDING_SCHEMA_VERSION = 1;

} // namespace minisql::sql
