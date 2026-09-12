// X25 绑定标识契约：RelationId/ColumnId/ExpressionId 稳定、作用域树正确、
// 受权对象按动作分派、绑定失败 fail-closed。
#include "minisql/sql/binding.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

minisql::catalog::Catalog fixture() {
    using namespace minisql;
    const auto statements = sql::parse(sql::tokenize(
        "CREATE TABLE Public_Records(id INT, note VARCHAR(32));"
        "CREATE TABLE Secret_Records(id INT, tag VARCHAR(32));"));
    catalog::Catalog catalog;
    for (const auto& statement : statements) catalog.create(statement);
    return catalog;
}

bool hasObject(const minisql::security::AccessRequest& request, const std::string& object,
               minisql::security::AccessAction action) {
    for (const auto& entry : request.objects)
        if (entry.object == object && entry.action == action) return true;
    return false;
}
}

int main() {
    using namespace minisql;
    using minisql::security::AccessAction;
    const auto catalog = fixture();

    {   // 别名不是对象；同一张表被引用两次得到两个 RelationId。
        const auto bound = sql::bindSource(
            "SELECT p.id FROM PUBLIC_RECORDS AS p JOIN secret_records AS s ON p.id = s.id;", catalog);
        require(bound.complete, "join binds");
        const auto request = bound.accessRequest();
        require(request.bound, "join request is bound");
        require(request.objects.size() == 2, "join binds exactly two objects");
        require(hasObject(request, "public_records", AccessAction::Select), "join binds public_records as select");
        require(hasObject(request, "secret_records", AccessAction::Select), "join binds secret_records as select");
        require(bound.scopes.relationCount() == 2, "join creates one relation per FROM item");
        const auto* first = bound.scopes.relation(static_cast<sql::RelationId>(1));
        require(first && first->alias == "p", "alias is the scope name");
        require(first->physicalName == "public_records", "physical name is catalog-normalized");
    }

    {   // 同一物理表的自连接：两个 RelationId，同名列不歧义地各归其主。
        const auto bound = sql::bindSource(
            "SELECT a.id FROM public_records AS a JOIN public_records AS b ON a.id = b.id;", catalog);
        require(bound.complete, "self join binds");
        require(bound.scopes.relationCount() == 2, "self join yields two relations");
        const auto left = bound.scopes.resolveColumn("a", "id", static_cast<sql::ScopeId>(1));
        const auto right = bound.scopes.resolveColumn("b", "id", static_cast<sql::ScopeId>(1));
        require(left && right, "qualified columns resolve on both sides");
        require(left->column != right->column, "same-name columns of two relations have distinct ColumnIds");
        require(left->relation != right->relation, "each side keeps its own RelationId");
        bool ambiguous = false;
        require(!bound.scopes.resolveColumn({}, "id", static_cast<sql::ScopeId>(1), &ambiguous),
                "unqualified id is ambiguous across the self join");
        require(ambiguous, "ambiguity is reported, not guessed");
        require(bound.accessRequest().objects.size() == 1, "self join binds one object");
    }

    {   // 派生表别名是作用域名，不是对象。
        const auto bound = sql::bindSource("SELECT * FROM (SELECT id FROM SECRET_RECORDS) AS hidden;", catalog);
        const auto request = bound.accessRequest();
        require(request.bound && request.objects.size() == 1, "derived table binds only its source");
        require(hasObject(request, "secret_records", AccessAction::Select), "derived source is the object");
        require(bound.scopes.scopeCount() == 2, "derived table opens an inner scope");
    }

    {   // CTE 名是作用域名；引用它不会产生对象，内部来源才会。
        const auto bound = sql::bindSource("WITH visible AS (SELECT id FROM SECRET_RECORDS) SELECT * FROM visible;", catalog);
        const auto request = bound.accessRequest();
        require(request.bound, "cte binds");
        require(request.objects.size() == 1 && hasObject(request, "secret_records", AccessAction::Select),
                "cte alias is not an access object");
        const auto* scope = bound.scopes.scope(static_cast<sql::ScopeId>(1));
        require(scope && scope->commonTableExpressions.size() == 1, "cte is registered as a scope name");
    }

    {   // 具名列 CTE，以及 CTE 与基表混用。
        const auto named = sql::bindSource(
            "WITH visible(id) AS (SELECT id FROM SECRET_RECORDS) SELECT id FROM visible;", catalog);
        require(named.accessRequest().objects.size() == 1, "named-column cte binds one object");
        const auto mixed = sql::bindSource(
            "WITH visible AS (SELECT id FROM PUBLIC_RECORDS) "
            "SELECT * FROM visible JOIN SECRET_RECORDS ON visible.id = SECRET_RECORDS.id;", catalog);
        const auto request = mixed.accessRequest();
        require(request.bound && request.objects.size() == 2, "cte joined with a base table binds both sources");
        require(hasObject(request, "public_records", AccessAction::Select) &&
                hasObject(request, "secret_records", AccessAction::Select), "both sources bind as select");
    }

    {   // 每个对象带自己的动作：子查询来源只要 SELECT，不再被保守地要求 DELETE。
        const auto bound = sql::bindSource(
            "DELETE FROM public_records WHERE id IN (SELECT id FROM secret_records);", catalog);
        const auto request = bound.accessRequest();
        require(request.bound, "delete with subquery binds");
        require(hasObject(request, "public_records", AccessAction::Delete), "delete target keeps the delete action");
        require(hasObject(request, "secret_records", AccessAction::Select), "subquery source only needs select");
        require(!hasObject(request, "secret_records", AccessAction::Delete), "subquery source is not escalated to delete");
    }

    {   // 字符串字面量里的表名不是对象。
        const auto bound = sql::bindSource("SELECT id FROM public_records WHERE note = 'secret_records';", catalog);
        const auto request = bound.accessRequest();
        require(request.objects.size() == 1 && hasObject(request, "public_records", AccessAction::Select),
                "string literals never become access objects");
    }

    {   // ExpressionId 稠密、稳定，并携带列解析结果。
        const auto bound = sql::bindSource("SELECT id FROM public_records WHERE id = 1;", catalog);
        require(bound.expressions.size() >= 3, "select list and predicate get expression ids");
        for (std::size_t index = 0; index < bound.expressions.size(); ++index)
            require(sql::rawId(bound.expressions[index].id) == index + 1, "expression ids are dense and 1-based");
        bool identifierBound = false;
        for (const auto& expression : bound.expressions)
            if (expression.node && expression.node->kind == "Identifier") {
                require(expression.column.has_value(), "identifier expressions carry a resolved ColumnId");
                require(expression.column->correlationDepth == 0, "uncorrelated reference has depth 0");
                identifierBound = true;
            }
        require(identifierBound, "at least one identifier was bound");
    }

    {   // EXPLAIN 前缀不改变绑定结果。
        const auto plain = sql::bindSource("SELECT id FROM public_records;", catalog);
        const auto explained = sql::bindSource("EXPLAIN ANALYZE SELECT id FROM public_records;", catalog);
        require(explained.complete, "explain binds");
        require(plain.accessRequest().objects == explained.accessRequest().objects, "EXPLAIN does not change objects");
    }

    {   // fail-closed：未知表、语法错误、递归 CTE 都不产生对象。
        for (const char* source : {"SELECT id FROM does_not_exist;",
                                   "SELEC id FROM public_records;",
                                   "WITH RECURSIVE loop AS (SELECT id FROM public_records) SELECT * FROM loop;"}) {
            const auto bound = sql::bindSource(source, catalog);
            const auto request = bound.accessRequest();
            require(!request.bound, "unbindable statement is not bound");
            require(request.objects.empty(), "unbindable statement yields no objects");
            require(!request.diagnostic.empty(), "unbindable statement explains why");
        }
    }

    {   // 事务语句无对象，动作为 Transaction。
        const auto bound = sql::bindSource("BEGIN;", catalog);
        const auto request = bound.accessRequest();
        require(request.bound && request.objects.empty(), "transaction control binds with no objects");
        require(request.statementAction == AccessAction::Transaction, "transaction control maps to the transaction permission");
    }

    {   // 相关子查询的外层引用由作用域归属判定，未限定的引用同样能认出来。
        const auto qualified = sql::bindSource(
            "SELECT id FROM public_records WHERE id IN (SELECT id FROM secret_records WHERE id = public_records.id);",
            catalog);
        require(qualified.complete, "qualified correlated subquery binds");
        bool sawQualified = false;
        for (const auto& entry : qualified.correlatedReferences) {
            require(!entry.second.empty(), "a recorded subquery has at least one outer reference");
            for (const auto& reference : entry.second) {
                require(reference.correlationDepth > 0, "outer reference crosses at least one scope");
                const auto* relation = qualified.scopes.relation(reference.relation);
                require(relation && relation->physicalName == "public_records",
                        "the outer reference resolves to the outer relation");
                sawQualified = true;
            }
        }
        require(sawQualified, "qualified outer reference recorded");

        // 同一条查询去掉限定符。此前 planner 靠对 subquerySql 重新分词、匹配
        // `IDENT . IDENT` 来猜外层引用，这种写法猜不到。
        const auto unqualified = sql::bindSource(
            "SELECT id FROM public_records WHERE note IN (SELECT tag FROM secret_records WHERE tag = note);",
            catalog);
        require(unqualified.complete, "unqualified correlated subquery binds");
        bool sawUnqualified = false;
        for (const auto& entry : unqualified.correlatedReferences)
            for (const auto& reference : entry.second) {
                const auto* relation = unqualified.scopes.relation(reference.relation);
                if (relation && relation->physicalName == "public_records") sawUnqualified = true;
            }
        require(sawUnqualified, "unqualified outer reference is recorded too");

        // 非相关子查询不应留下任何外层引用记录。
        const auto plain = sql::bindSource(
            "SELECT id FROM public_records WHERE id IN (SELECT id FROM secret_records);", catalog);
        require(plain.complete, "uncorrelated subquery binds");
        require(plain.correlatedReferences.empty(), "an uncorrelated subquery records no outer references");
    }

    {   // 序列化是稳定的：同一 SQL 两次绑定产出相同文档。
        const auto first = sql::serializeBinding(sql::bindSource("SELECT id FROM public_records;", catalog));
        const auto second = sql::serializeBinding(sql::bindSource("SELECT id FROM public_records;", catalog));
        require(first == second, "binding serialization is deterministic");
        require(first.at("schemaVersion") == sql::BINDING_SCHEMA_VERSION, "binding document is versioned");
    }

    std::cout << "binding identity contract passed\n";
    return 0;
}
