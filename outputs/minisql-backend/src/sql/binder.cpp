#include "minisql/sql/binding.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace minisql::sql {
namespace {

std::string fold(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::pair<std::string, std::string> splitQualified(const std::string& value) {
    const auto dot = value.find('.');
    if (dot == std::string::npos) return {std::string{}, value};
    return {value.substr(0, dot), value.substr(dot + 1)};
}

// 绑定失败：对象集合无法闭合。调用方一律 fail-closed，不存在文本扫描兜底。
// 携带错误码与位置：拒绝时按第十九章回报真实原因（表不存在 → Catalog），
// 而不是把「SQL 检查失败」统一伪装成权限错误。
struct BindFailure {
    std::string reason;
    ErrorCode code = ErrorCode::Semantic;
    SourceLocation location{};
};

[[noreturn]] void failBind(const std::string& reason, ErrorCode code = ErrorCode::Semantic,
                           SourceLocation location = {}) {
    throw BindFailure{reason, code, location};
}

using security::AccessAction;

bool transactionKind(const std::string& kind) {
    return kind == "Begin" || kind == "Commit" || kind == "Rollback" || kind == "RollbackTo" ||
           kind == "Savepoint" || kind == "ReleaseSavepoint";
}

AccessAction statementActionFor(const std::string& kind) {
    if (kind == "Select") return AccessAction::Select;
    if (kind == "Insert") return AccessAction::Insert;
    if (kind == "Update") return AccessAction::Update;
    if (kind == "Delete") return AccessAction::Delete;
    if (kind == "CreateTable" || kind == "CreateIndex") return AccessAction::Create;
    if (kind == "DropIndex") return AccessAction::Drop;
    if (kind == "Checkpoint") return AccessAction::Checkpoint;
    if (transactionKind(kind)) return AccessAction::Transaction;
    return AccessAction::Compile;
}

class Binder {
public:
    Binder(const catalog::Catalog& catalog, BindResult& result) : catalog_(catalog), result_(result) {}

    void run(const std::vector<const Statement*>& statements) {
        for (const auto* statement : statements) {
            if (!statement) failBind("null statement");
            if (statement->invalid) failBind("statement did not parse cleanly");
            result_.topLevel.push_back(result_.statements.size());
            bindStatement(*statement, ScopeId::Invalid, 0);
            // 批内 DDL 推进目录快照：后一条语句才能解析到刚建的表/索引。
            // 授权对象集合因此与实际执行顺序一致；失败留给执行器报错。
            applyDdl(*statement);
        }
        if (!statements.empty()) result_.statementAction = statementActionFor(statements.front()->kind);
        result_.complete = true;
    }

private:
    static constexpr std::size_t kMaxDepth = 64;

    catalog::Catalog catalog_;
    BindResult& result_;
    std::size_t currentStatement_ = 0;  // 表达式归属的 BoundStatement 下标

    void applyDdl(const Statement& statement) {
        try {
            if (statement.kind == "CreateTable") catalog_.create(statement);
            else if (statement.kind == "CreateIndex") catalog_.createIndex(statement);
            else if (statement.kind == "DropIndex") catalog_.dropIndex(statement);
        } catch (const MiniSqlError&) {
            // 目录推进失败不在这里失败：后续语句会因此无法闭合而 fail-closed，
            // 真实错误由执行器在运行阶段给出。
        }
    }

    struct StatementScope {
        Binder& binder;
        std::size_t previous;
        StatementScope(Binder& owner, std::size_t index) : binder(owner), previous(owner.currentStatement_) {
            binder.currentStatement_ = index;
        }
        ~StatementScope() { binder.currentStatement_ = previous; }
    };

    void addObject(const std::string& object, AccessAction action) {
        if (object.empty()) return;
        const auto name = fold(object);
        for (const auto& existing : result_.objects)
            if (existing.object == name && existing.action == action) return;
        result_.objects.push_back({name, action});
    }

    ScopeId bindStatement(const Statement& statement, ScopeId parent, std::size_t depth) {
        if (depth > kMaxDepth) failBind("binding depth exceeded");
        const auto scope = result_.scopes.createScope(parent);
        BoundStatement bound;
        bound.kind = statement.kind;
        bound.scope = scope;
        bound.location = statement.location;
        const auto statementIndex = result_.statements.size();
        result_.statements.push_back(bound);
        result_.statementIndex.emplace(&statement, statementIndex);
        const StatementScope active(*this, statementIndex);

        if (transactionKind(statement.kind) || statement.kind == "Checkpoint") return scope;

        // WITH 先于 FROM 绑定；后一个 CTE 可以引用前一个。
        for (const auto& cte : statement.ctes) bindCte(cte, scope, depth);

        if (statement.kind == "Select") {
            bindFrom(statement, scope, depth);
        } else if (statement.kind == "CreateTable") {
            addObject(statement.table, AccessAction::Create);
            for (const auto& key : statement.foreignKeys) addObject(key.table, AccessAction::Create);
            for (const auto& column : statement.columns)
                if (column.references) addObject(column.references->first, AccessAction::Create);
        } else if (statement.kind == "CreateIndex" || statement.kind == "DropIndex") {
            const auto action = statement.kind == "CreateIndex" ? AccessAction::Create : AccessAction::Drop;
            if (!statement.table.empty()) {
                result_.statements[statementIndex].target = bindBaseRelation(statement.table, {}, scope, statement.location, action);
            }
        } else if (statement.fromSubquery && (statement.kind == "Update" || statement.kind == "Delete")) {
            bindFrom(statement, scope, depth);
            const auto* level = result_.scopes.scope(scope);
            if (level && !level->relations.empty()) result_.statements[statementIndex].target = level->relations.front();
            if (!statement.fromSubquery->table.empty()) addObject(statement.fromSubquery->table, statementActionFor(statement.kind));
        } else {
            // Insert / Update / Delete：目标关系承担写动作。
            const auto action = statementActionFor(statement.kind);
            if (statement.table.empty()) failBind("statement has no target relation");
            result_.statements[statementIndex].target =
                bindBaseRelation(statement.table, statement.tableAlias, scope, statement.location, action);
        }

        bindClauses(statement, scope, depth);
        return scope;
    }

    void bindCte(const CommonTableExpr& cte, ScopeId scope, std::size_t depth) {
        if (!cte.query) failBind("common table expression has no query");
        const auto inner = bindStatement(*cte.query, scope, depth + 1);
        BoundRelation relation;
        relation.kind = RelationKind::CommonTableExpression;
        relation.name = cte.name;
        relation.alias = cte.name;
        relation.scope = scope;
        relation.innerScope = inner;
        relation.location = cte.location;
        // 作用域名而非 FROM 项：FROM 引用它时才实例化成本层的一个关系。
        const auto id = result_.scopes.addCommonTableExpression(scope, std::move(relation));
        if (!cte.columns.empty()) {
            for (std::size_t ordinal = 0; ordinal < cte.columns.size(); ++ordinal)
                result_.scopes.addColumn(id, BoundColumn{{}, id, ordinal, cte.columns[ordinal], std::string{}, true});
        } else {
            copyOutputColumns(*cte.query, inner, id);
        }
    }

    void bindFrom(const Statement& statement, ScopeId scope, std::size_t depth) {
        if (statement.fromSubquery) {
            const auto inner = bindStatement(*statement.fromSubquery, scope, depth + 1);
            BoundRelation relation;
            relation.kind = RelationKind::DerivedTable;
            relation.name = statement.tableAlias;
            relation.alias = statement.tableAlias;
            relation.scope = scope;
            relation.innerScope = inner;
            relation.location = statement.location;
            const auto id = result_.scopes.addRelation(scope, std::move(relation));
            copyOutputColumns(*statement.fromSubquery, inner, id);
        } else if (!statement.table.empty()) {
            bindFromName(statement.table, statement.tableAlias, scope, statement.location, AccessAction::Select);
        }
        for (const auto& join : statement.joins) {
            if (join.table.empty()) failBind("join has no relation name");
            bindFromName(join.table, join.alias, scope, statement.location, AccessAction::Select);
        }
    }

    // 一个 FROM/JOIN 名字要么解析到作用域里的 CTE（不产生受权对象），
    // 要么解析到 Catalog 中的物理表（产生受权对象）。两者都不成立即绑定失败。
    RelationId bindFromName(const std::string& name, const std::string& alias, ScopeId scope,
                            SourceLocation location, AccessAction action) {
        if (const auto* cte = result_.scopes.resolveRelation(name, scope);
            cte && cte->kind == RelationKind::CommonTableExpression) {
            // addRelation 会让 relations_ 重新分配，`cte` 随之失效：
            // 先把需要的字段复制出来，再建关系。
            BoundRelation relation;
            relation.kind = RelationKind::CommonTableExpression;
            relation.name = cte->name;
            relation.alias = alias.empty() ? cte->name : alias;
            relation.scope = scope;
            relation.innerScope = cte->innerScope;
            relation.location = location;
            const auto sourceColumns = cte->columns;
            const auto id = result_.scopes.addRelation(scope, std::move(relation));
            for (const auto column : sourceColumns) {
                // column(...) 的返回值同样只在下一次 addColumn 之前有效。
                const auto* source = result_.scopes.column(column);
                if (!source) continue;
                const BoundColumn copied{{}, id, source->ordinal, source->name, source->type, source->nullable};
                result_.scopes.addColumn(id, copied);
            }
            return id;
        }
        return bindBaseRelation(name, alias, scope, location, action);
    }

    RelationId bindBaseRelation(const std::string& name, const std::string& alias, ScopeId scope,
                                SourceLocation location, AccessAction action) {
        const auto* table = catalog_.find(name);
        if (!table) failBind("unknown relation '" + name + "'", ErrorCode::Catalog, location);
        BoundRelation relation;
        relation.kind = RelationKind::BaseTable;
        relation.name = name;
        relation.alias = alias.empty() ? table->name : alias;
        relation.physicalName = fold(table->name);
        relation.scope = scope;
        relation.location = location;
        const auto id = result_.scopes.addRelation(scope, std::move(relation));
        for (std::size_t ordinal = 0; ordinal < table->columns.size(); ++ordinal) {
            const auto& column = table->columns[ordinal];
            result_.scopes.addColumn(id, BoundColumn{{}, id, ordinal, column.name, column.type, column.nullable});
        }
        addObject(table->name, action);
        return id;
    }

    // 派生表 / CTE 的输出列名：能静态判定的直接抄写，无法判定（含通配符或
    // 复杂表达式）时留空——列身份缺失不影响对象绑定是否闭合。
    void copyOutputColumns(const Statement& query, ScopeId innerScope, RelationId target) {
        const auto* inner = result_.scopes.scope(innerScope);
        if (!inner) return;
        std::size_t ordinal = 0;
        for (const auto& item : query.selectItems) {
            if (!item.expression) return;
            if (item.expression->kind == "Wildcard") {
                // 先快照来源列表：addColumn 会改动 ScopeTree 的向量。
                std::vector<ColumnId> sources;
                for (const auto relation : inner->relations)
                    if (const auto* source = result_.scopes.relation(relation))
                        sources.insert(sources.end(), source->columns.begin(), source->columns.end());
                for (const auto column : sources) {
                    const auto* bound = result_.scopes.column(column);
                    if (!bound) continue;
                    const BoundColumn copied{{}, target, ordinal++, bound->name, bound->type, bound->nullable};
                    result_.scopes.addColumn(target, copied);
                }
                continue;
            }
            std::string name = item.alias;
            std::string type;
            bool nullable = true;
            if (name.empty() && item.expression->kind == "Identifier") {
                const auto [qualifier, column] = splitQualified(item.expression->value);
                name = column;
                if (const auto reference = result_.scopes.resolveColumn(qualifier, column, innerScope))
                    if (const auto* bound = result_.scopes.column(reference->column)) { type = bound->type; nullable = bound->nullable; }
            }
            if (name.empty()) { ++ordinal; continue; }
            result_.scopes.addColumn(target, BoundColumn{{}, target, ordinal++, name, type, nullable});
        }
    }

    void bindClauses(const Statement& statement, ScopeId scope, std::size_t depth) {
        const auto visit = [&](const std::shared_ptr<Expr>& expression) {
            bindExpression(expression, scope, ExpressionId::Invalid, depth, statement.kind);
        };
        visit(statement.where);
        visit(statement.having);
        for (const auto& item : statement.selectItems) visit(item.expression);
        for (const auto& item : statement.orderBy) visit(item.expression);
        for (const auto& item : statement.assignments) visit(item.expression);
        for (const auto& item : statement.groupBy) visit(item);
        for (const auto& item : statement.checks) visit(item);
        for (const auto& item : statement.valueExpressions) visit(item);
        for (const auto& row : statement.valueRows)
            for (const auto& item : row) visit(item);
        for (const auto& join : statement.joins) visit(join.on);
    }

    // 子查询体内引用到的、归属于 `scope` 或其祖先的列，就是这个子查询的外层引用。
    void recordCorrelated(const Expr* node, ScopeId scope, std::size_t nestedBegin) {
        std::vector<ColumnRef> outer;
        std::vector<ColumnId> seen;
        for (std::size_t index = nestedBegin; index < result_.expressions.size(); ++index) {
            const auto& bound = result_.expressions[index];
            if (!bound.column) continue;
            const auto* relation = result_.scopes.relation(bound.column->relation);
            if (!relation || !result_.scopes.atOrAbove(relation->scope, scope)) continue;
            if (std::find(seen.begin(), seen.end(), bound.column->column) != seen.end()) continue;
            seen.push_back(bound.column->column);
            outer.push_back(*bound.column);
        }
        if (!outer.empty()) result_.correlatedReferences.emplace(node, std::move(outer));
    }

    void bindExpression(const std::shared_ptr<Expr>& expression, ScopeId scope, ExpressionId parent,
                        std::size_t depth, const std::string& statementKind) {
        if (!expression) return;
        if (depth > kMaxDepth) failBind("expression depth exceeded");

        BoundExpression bound;
        bound.id = static_cast<ExpressionId>(result_.expressions.size() + 1);
        bound.node = expression.get();
        bound.scope = scope;
        bound.parent = parent;
        if (expression->kind == "Identifier") {
            const auto [qualifier, name] = splitQualified(expression->value);
            bool ambiguous = false;
            // 列解析失败不阻断对象绑定；语义层会在计划阶段给出更精确的错误。
            bound.column = result_.scopes.resolveColumn(qualifier, name, scope, &ambiguous);
            if (ambiguous) bound.column.reset();
        }
        result_.expressions.push_back(bound);
        const auto id = bound.id;
        result_.expressionIds.emplace(expression.get(), id);
        if (currentStatement_ < result_.statements.size())
            result_.statements[currentStatement_].expressions.push_back(id);

        // 子查询节点：记录它实际引用到的外层列。判定用作用域归属，不是列名文本，
        // 因此未限定的外层引用同样能被认出来。
        const auto nestedBegin = result_.expressions.size();
        const bool subqueryNode = expression->subquery || !expression->subquerySql.empty();
        if (expression->subquery) {
            bindStatement(*expression->subquery, scope, depth + 1);
        } else if (!expression->subquerySql.empty()) {
            // X09 过渡期的文本子查询：在这里一次性结构化，绝不留给权限层去扫描。
            auto nested = expression->subquerySql;
            const auto last = nested.find_last_not_of(" \t\r\n");
            if (last == std::string::npos || nested[last] != ';') nested += ';';
            try {
                for (auto& statement : parse(tokenize(nested))) {
                    // 解析出来的语句必须活得比 BindResult 久：BoundExpression::node
                    // 与 statementIndex 都持有指向它们的裸指针。
                    const auto owned = std::make_shared<Statement>(std::move(statement));
                    result_.owned.push_back(owned);
                    bindStatement(*owned, scope, depth + 1);
                }
            } catch (const MiniSqlError& error) {
                // 嵌套子查询的真实错误码要透传：否则「子查询里的表不存在」会被
                // 归成笼统的语义错误，丢失可诊断性。
                failBind(std::string("nested subquery did not bind: ") + error.what(), error.code(), error.location());
            }
        }
        if (subqueryNode) recordCorrelated(expression.get(), scope, nestedBegin);
        bindExpression(expression->left, scope, id, depth + 1, statementKind);
        bindExpression(expression->right, scope, id, depth + 1, statementKind);
    }
};

} // namespace

ScopeId ScopeTree::createScope(ScopeId parent) {
    Scope scope;
    scope.id = static_cast<ScopeId>(scopes_.size() + 1);
    scope.parent = parent;
    scopes_.push_back(std::move(scope));
    return scopes_.back().id;
}

RelationId ScopeTree::addRelation(ScopeId scope, BoundRelation relation) {
    relation.id = static_cast<RelationId>(relations_.size() + 1);
    relation.scope = scope;
    const auto id = relation.id;
    relations_.push_back(std::move(relation));
    if (validId(scope) && rawId(scope) <= scopes_.size()) scopes_[rawId(scope) - 1].relations.push_back(id);
    return id;
}

RelationId ScopeTree::addCommonTableExpression(ScopeId scope, BoundRelation relation) {
    relation.id = static_cast<RelationId>(relations_.size() + 1);
    relation.scope = scope;
    const auto id = relation.id;
    relations_.push_back(std::move(relation));
    if (validId(scope) && rawId(scope) <= scopes_.size())
        scopes_[rawId(scope) - 1].commonTableExpressions.push_back(id);
    return id;
}

ColumnId ScopeTree::addColumn(RelationId relation, BoundColumn column) {
    column.id = static_cast<ColumnId>(columns_.size() + 1);
    column.relation = relation;
    const auto id = column.id;
    columns_.push_back(std::move(column));
    if (validId(relation) && rawId(relation) <= relations_.size()) relations_[rawId(relation) - 1].columns.push_back(id);
    return id;
}

const Scope* ScopeTree::scope(ScopeId id) const {
    if (!validId(id) || rawId(id) > scopes_.size()) return nullptr;
    return &scopes_[rawId(id) - 1];
}

const BoundRelation* ScopeTree::relation(RelationId id) const {
    if (!validId(id) || rawId(id) > relations_.size()) return nullptr;
    return &relations_[rawId(id) - 1];
}

const BoundColumn* ScopeTree::column(ColumnId id) const {
    if (!validId(id) || rawId(id) > columns_.size()) return nullptr;
    return &columns_[rawId(id) - 1];
}

std::optional<ColumnRef> ScopeTree::resolveColumn(const std::string& qualifier, const std::string& name,
                                                  ScopeId from, bool* ambiguous) const {
    if (ambiguous) *ambiguous = false;
    const auto wanted = fold(name);
    const auto wantedQualifier = fold(qualifier);
    std::size_t depth = 0;
    for (auto current = from; validId(current); ++depth) {
        const auto* level = scope(current);
        if (!level) break;
        std::optional<ColumnRef> found;
        for (const auto relationId : level->relations) {
            const auto* relation = this->relation(relationId);
            if (!relation) continue;
            if (!wantedQualifier.empty() && fold(relation->alias) != wantedQualifier &&
                fold(relation->name) != wantedQualifier && fold(relation->physicalName) != wantedQualifier)
                continue;
            for (const auto columnId : relation->columns) {
                const auto* column = this->column(columnId);
                if (!column || fold(column->name) != wanted) continue;
                if (found) { if (ambiguous) *ambiguous = true; return std::nullopt; }
                found = ColumnRef{columnId, relationId, depth};
            }
        }
        if (found) return found;
        current = level->parent;
    }
    return std::nullopt;
}

const BoundRelation* ScopeTree::resolveRelation(const std::string& name, ScopeId from) const {
    const auto wanted = fold(name);
    for (auto current = from; validId(current);) {
        const auto* level = scope(current);
        if (!level) break;
        for (const auto id : level->commonTableExpressions)
            if (const auto* relation = this->relation(id); relation && fold(relation->alias) == wanted) return relation;
        for (const auto id : level->relations)
            if (const auto* relation = this->relation(id); relation && fold(relation->alias) == wanted) return relation;
        current = level->parent;
    }
    return nullptr;
}

const BoundExpression* BindResult::expression(ExpressionId id) const {
    if (!validId(id) || rawId(id) > expressions.size()) return nullptr;
    return &expressions[rawId(id) - 1];
}

bool ScopeTree::atOrAbove(ScopeId candidate, ScopeId from) const {
    if (!validId(candidate) || !validId(from)) return false;
    for (auto current = from; validId(current);) {
        if (current == candidate) return true;
        const auto* level = scope(current);
        if (!level) break;
        current = level->parent;
    }
    return false;
}

const std::vector<ColumnRef>& BindResult::correlatedFor(const Expr* subquery) const {
    static const std::vector<ColumnRef> none;
    const auto found = correlatedReferences.find(subquery);
    return found == correlatedReferences.end() ? none : found->second;
}

const BoundStatement* BindResult::statementFor(const Statement* node) const {
    const auto found = statementIndex.find(node);
    return found == statementIndex.end() ? nullptr : &statements[found->second];
}

ExpressionId BindResult::idFor(const Expr* node) const {
    const auto found = expressionIds.find(node);
    return found == expressionIds.end() ? ExpressionId::Invalid : found->second;
}

security::AccessRequest BindResult::accessRequest() const {
    security::AccessRequest request;
    request.statementAction = statementAction;
    request.bound = complete;
    request.diagnostic = diagnostic;
    request.failureCode = failureCode;
    request.failureLocation = failureLocation;
    if (complete) request.objects = objects;
    return request;
}

std::vector<std::string> BindResult::accessObjectNames() const {
    std::vector<std::string> names;
    for (const auto& object : objects)
        if (std::find(names.begin(), names.end(), object.object) == names.end()) names.push_back(object.object);
    return names;
}

namespace {
void runBinder(const std::vector<const Statement*>& statements, const catalog::Catalog& catalog, BindResult& result) {
    Binder binder(catalog, result);
    try {
        binder.run(statements);
    } catch (const BindFailure& failure) {
        result.complete = false;
        result.diagnostic = failure.reason;
        result.failureCode = failure.code;
        result.failureLocation = failure.location;
        result.objects.clear();
    } catch (const MiniSqlError& error) {
        result.complete = false;
        result.diagnostic = error.what();
        result.failureCode = error.code();
        result.failureLocation = error.location();
        result.objects.clear();
    }
}
}

BindResult bind(const std::vector<Statement>& statements, const catalog::Catalog& catalog) {
    BindResult result;
    std::vector<const Statement*> pointers;
    pointers.reserve(statements.size());
    for (const auto& statement : statements) pointers.push_back(&statement);
    runBinder(pointers, catalog, result);
    return result;
}

BindResult bindStatements(const std::vector<const Statement*>& statements, const catalog::Catalog& catalog) {
    BindResult result;
    runBinder(statements, catalog, result);
    return result;
}

// 取得语句所有权后再绑定：BoundExpression::node 与 statementIndex 持有裸指针，
// 解析出来的 AST 必须活得比 BindResult 久。
BindResult bindOwned(std::vector<Statement> statements, const catalog::Catalog& catalog) {
    BindResult result;
    std::vector<const Statement*> pointers;
    pointers.reserve(statements.size());
    for (auto& statement : statements) {
        result.owned.push_back(std::make_shared<Statement>(std::move(statement)));
        pointers.push_back(result.owned.back().get());
    }
    runBinder(pointers, catalog, result);
    return result;
}

BindResult bindSource(const std::string& source, const catalog::Catalog& catalog) {
    try {
        auto tokens = tokenize(source);
        const auto leading = [&](const char* word) {
            if (tokens.empty()) return false;
            return fold(tokens.front().lexeme) == word;
        };
        if (leading("explain")) {
            tokens.erase(tokens.begin());
            if (leading("analyze")) tokens.erase(tokens.begin());
        }
        return bindOwned(parse(tokens), catalog);
    } catch (const MiniSqlError& error) {
        // 词法/语法错误在这里被收敛成「未绑定」。错误码必须一起带走：否则
        // 启用权限目录时语法错误会被鉴权层报成 403，禁用权限目录时却是
        // 422/2002——同一句 SQL 因是否配置权限而得到不同的状态码。
        BindResult result;
        result.complete = false;
        result.diagnostic = error.what();
        result.failureCode = error.code();
        result.failureLocation = error.location();
        return result;
    }
}

nlohmann::json serializeBinding(const BindResult& result) {
    const auto relationKind = [](RelationKind kind) {
        switch (kind) {
            case RelationKind::BaseTable: return "baseTable";
            case RelationKind::DerivedTable: return "derivedTable";
            case RelationKind::CommonTableExpression: return "commonTableExpression";
        }
        return "baseTable";
    };
    nlohmann::json scopes = nlohmann::json::array();
    for (const auto& scope : result.scopes.scopes()) {
        nlohmann::json relations = nlohmann::json::array();
        for (const auto id : scope.relations) relations.push_back(rawId(id));
        nlohmann::json ctes = nlohmann::json::array();
        for (const auto id : scope.commonTableExpressions) ctes.push_back(rawId(id));
        scopes.push_back({{"scopeId", rawId(scope.id)}, {"parent", rawId(scope.parent)},
                          {"relations", relations}, {"commonTableExpressions", ctes}});
    }
    nlohmann::json relations = nlohmann::json::array();
    for (const auto& relation : result.scopes.relations()) {
        nlohmann::json columns = nlohmann::json::array();
        for (const auto id : relation.columns) columns.push_back(rawId(id));
        relations.push_back({{"relationId", rawId(relation.id)}, {"kind", relationKind(relation.kind)},
                             {"name", relation.name}, {"alias", relation.alias},
                             {"physicalName", relation.physicalName}, {"scope", rawId(relation.scope)},
                             {"innerScope", rawId(relation.innerScope)}, {"columns", columns}});
    }
    nlohmann::json columns = nlohmann::json::array();
    for (const auto& column : result.scopes.columns())
        columns.push_back({{"columnId", rawId(column.id)}, {"relation", rawId(column.relation)},
                           {"ordinal", column.ordinal}, {"name", column.name},
                           {"type", column.type}, {"nullable", column.nullable}});
    nlohmann::json expressions = nlohmann::json::array();
    for (const auto& expression : result.expressions) {
        nlohmann::json node{{"expressionId", rawId(expression.id)}, {"scope", rawId(expression.scope)},
                            {"parent", rawId(expression.parent)},
                            {"kind", expression.node ? expression.node->kind : std::string{}}};
        if (expression.column) {
            node["columnId"] = rawId(expression.column->column);
            node["relationId"] = rawId(expression.column->relation);
            node["correlationDepth"] = expression.column->correlationDepth;
        }
        expressions.push_back(std::move(node));
    }
    nlohmann::json objects = nlohmann::json::array();
    for (const auto& object : result.objects)
        objects.push_back({{"object", object.object}, {"action", security::permissionName(object.action)}});
    nlohmann::json statements = nlohmann::json::array();
    for (const auto& statement : result.statements)
        statements.push_back({{"kind", statement.kind}, {"scope", rawId(statement.scope)},
                              {"target", rawId(statement.target)}});
    return {{"schemaVersion", BINDING_SCHEMA_VERSION}, {"complete", result.complete},
            {"diagnostic", result.diagnostic}, {"statements", statements}, {"scopes", scopes},
            {"relations", relations}, {"columns", columns}, {"expressions", expressions},
            {"accessObjects", objects}};
}

} // namespace minisql::sql
