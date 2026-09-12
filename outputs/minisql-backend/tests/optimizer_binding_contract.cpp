// X25 阶段三契约：优化器消费稳定身份。
// 关注点是「身份不随槽位平移而变化」，以及归属判断不再依赖 output 宽度。
#include "minisql/optimizer/optimizer.hpp"
#include "minisql/sql/planner.hpp"
#include <iostream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}

minisql::catalog::Catalog fixture() {
    using namespace minisql;
    catalog::Catalog catalog;
    for (const auto& statement : sql::parse(sql::tokenize(
             "CREATE TABLE a(id INT, x INT, note VARCHAR(16));"
             "CREATE TABLE b(id INT, y INT, tag VARCHAR(16));")))
        catalog.create(statement);
    return catalog;
}

minisql::sql::LogicalPlan plan(const minisql::catalog::Catalog& catalog, const std::string& sql) {
    using namespace minisql;
    auto plans = sql::compilePlans(sql::parse(sql::tokenize(sql)), catalog);
    require(plans.size() == 1, "one plan per statement");
    return plans.front();
}

const minisql::sql::LogicalPlan* findKind(const minisql::sql::LogicalPlan& node, const std::string& kind) {
    if (node.kind == kind) return &node;
    for (const auto& child : node.children)
        if (const auto* found = findKind(child, kind)) return found;
    return nullptr;
}

// 收集一棵计划里所有 Filter 的谓词。
void collectFilters(const minisql::sql::LogicalPlan& node, std::vector<const minisql::sql::LogicalPlan*>& out) {
    if (node.kind == "Filter") out.push_back(&node);
    for (const auto& child : node.children) collectFilters(child, out);
}

void collectIdentifiers(const nlohmann::json& expression, std::vector<nlohmann::json>& out) {
    if (!expression.is_object()) return;
    if (expression.value("kind", "") == "Identifier") out.push_back(expression);
    if (expression.contains("left")) collectIdentifiers(expression.at("left"), out);
    if (expression.contains("right")) collectIdentifiers(expression.at("right"), out);
}
}

int main() {
try {
    using namespace minisql;
    const auto catalog = fixture();

    {   // 谓词下推：推到右侧的项槽位被重基，但稳定身份原样保留。
        const auto source = plan(catalog, "SELECT a.x, b.y FROM a JOIN b ON a.id = b.id WHERE b.y > 10;");
        std::vector<const sql::LogicalPlan*> before;
        collectFilters(source, before);
        std::vector<nlohmann::json> beforeRefs;
        for (const auto* filter : before) collectIdentifiers(filter->predicate, beforeRefs);
        require(beforeRefs.size() == 1, "one column reference in the original predicate");
        const auto beforeSlot = beforeRefs.front().at("columnId").get<std::size_t>();
        const auto beforeBinding = beforeRefs.front().value("binding", std::uint32_t{0});
        require(beforeBinding != 0, "planner stamped a stable binding on the predicate");
        require(beforeSlot >= 3, "b.y lives in the right half of the joined row");

        const auto optimized = optimizer::optimize({source});
        require(optimized.plans.size() == 1, "optimizer keeps one plan");
        std::vector<const sql::LogicalPlan*> after;
        collectFilters(optimized.plans.front(), after);
        require(!after.empty(), "predicate survived");
        std::vector<nlohmann::json> afterRefs;
        for (const auto* filter : after) collectIdentifiers(filter->predicate, afterRefs);
        require(afterRefs.size() == 1, "still one column reference");
        const auto afterSlot = afterRefs.front().at("columnId").get<std::size_t>();
        const auto afterBinding = afterRefs.front().value("binding", std::uint32_t{0});
        require(afterSlot < beforeSlot, "the slot was rebased onto the right child's row");
        require(afterBinding == beforeBinding, "the stable binding survived the slot rebase");
    }

    {   // 自连接：两侧同名同表，只有关系身份能把谓词归到正确的一侧。
        const auto source = plan(catalog,
            "SELECT p.x FROM a AS p JOIN a AS q ON p.id = q.id WHERE q.x > 10;");
        const auto* join = findKind(source, "NestedLoopJoin");
        require(join != nullptr, "self join planned");
        require(join->children.size() == 2, "join has two children");
        std::set<std::uint32_t> left, right;
        for (const auto& column : join->children[0].output) if (column.relation) left.insert(column.relation);
        for (const auto& column : join->children[1].output) if (column.relation) right.insert(column.relation);
        require(!left.empty() && !right.empty(), "both sides carry relation identity");
        for (const auto id : left) require(!right.contains(id), "the two sides of a self join are distinguishable");

        const auto optimized = optimizer::optimize({source});
        std::vector<const sql::LogicalPlan*> filters;
        collectFilters(optimized.plans.front(), filters);
        require(!filters.empty(), "self-join predicate survived");
        // q.x 只引用右侧关系，必须落在右子树上。
        bool attributedToRight = false;
        for (const auto* filter : filters) {
            std::vector<nlohmann::json> refs;
            collectIdentifiers(filter->predicate, refs);
            if (refs.empty()) continue;
            const auto relation = refs.front().value("relation", std::uint32_t{0});
            require(relation != 0, "the pushed predicate kept its relation identity");
            require(!left.contains(relation), "q.x is never attributed to the p side");
            if (right.contains(relation)) attributedToRight = true;
        }
        require(attributedToRight, "the predicate is attributed to the aliased side it actually references");
    }

    {   // 列裁剪按身份进行；保留下来的列身份不变。
        const auto source = plan(catalog, "SELECT x FROM a WHERE id > 1;");
        const auto* scanBefore = findKind(source, "SeqScan");
        require(scanBefore != nullptr && scanBefore->output.size() == 3, "scan starts with every column");
        std::set<std::uint32_t> keptBefore;
        for (const auto& column : scanBefore->output)
            if (column.name == "x" || column.name == "id") keptBefore.insert(column.binding);
        require(keptBefore.size() == 2 && !keptBefore.contains(0), "referenced columns carry identity");

        const auto optimized = optimizer::optimize({source});
        const auto* scanAfter = findKind(optimized.plans.front(), "SeqScan");
        require(scanAfter != nullptr, "scan survived");
        require(scanAfter->output.size() == 2, "the unreferenced column was pruned");
        std::set<std::uint32_t> keptAfter;
        for (const auto& column : scanAfter->output) keptAfter.insert(column.binding);
        require(keptAfter == keptBefore, "pruning keeps exactly the referenced identities, unrenumbered");
        for (const auto& column : scanAfter->output)
            require(column.name != "note", "the pruned column is gone");
    }

    {   // 裁剪不会重新编号：留下的列槽位仍指向扫描行里的原位置。
        const auto source = plan(catalog, "SELECT note FROM a;");
        const auto optimized = optimizer::optimize({source});
        const auto* scan = findKind(optimized.plans.front(), "SeqScan");
        require(scan != nullptr && scan->output.size() == 1, "only the projected column remains");
        require(scan->output.front().columnId == 2, "the surviving slot is not renumbered");
        require(scan->output.front().binding != 0, "the surviving column keeps its identity");
    }

    {   // 序列化往返保留身份。
        const auto source = plan(catalog, "SELECT a.x FROM a JOIN b ON a.id = b.id;");
        const auto document = sql::serializePlans({source});
        const auto restored = sql::deserializePlans(document);
        require(restored.size() == 1, "round trip keeps one plan");
        require(sql::serializePlans(restored) == document, "plan document round trips byte for byte");
        const auto* join = findKind(restored.front(), "NestedLoopJoin");
        require(join != nullptr, "join survived the round trip");
        for (const auto& column : join->output)
            require(column.binding != 0 && column.relation != 0, "identity survived serialization");
    }

    {   // 相关子查询的 GROUP BY 检查：外层引用来自绑定器，不再靠对 subquerySql
        // 重新分词猜 `IDENT . IDENT`。未限定的外层引用此前猜不到，会被放行。
        const auto compiles = [&](const std::string& sql) {
            try {
                sql::compilePlans(sql::parse(sql::tokenize(sql)), catalog);
                return true;
            } catch (const MiniSqlError&) {
                return false;
            }
        };
        require(!compiles("SELECT a.id, (SELECT COUNT(*) FROM b WHERE b.id = a.x) FROM a GROUP BY a.id;"),
                "a qualified ungrouped outer reference is rejected");
        require(!compiles("SELECT a.id, (SELECT COUNT(*) FROM b WHERE b.id = x) FROM a GROUP BY a.id;"),
                "an unqualified ungrouped outer reference is rejected too");
        require(compiles("SELECT a.id, (SELECT COUNT(*) FROM b WHERE b.id = a.id) FROM a GROUP BY a.id;"),
                "a grouped outer reference still compiles");
        require(compiles("SELECT a.id, (SELECT COUNT(*) FROM b) FROM a GROUP BY a.id;"),
                "an uncorrelated subquery is unaffected");

        // 外层引用带稳定身份，并在聚合下降后被重映射到分组槽位。
        const auto source = plan(catalog, "SELECT a.id, (SELECT COUNT(*) FROM b WHERE b.id = a.id) FROM a GROUP BY a.id;");
        std::function<void(const nlohmann::json&, std::vector<nlohmann::json>&)> collect =
            [&](const nlohmann::json& node, std::vector<nlohmann::json>& out) {
                if (!node.is_object()) return;
                if (node.contains("outerReferences") && node.at("outerReferences").is_array())
                    for (const auto& item : node.at("outerReferences")) out.push_back(item);
                if (node.contains("left")) collect(node.at("left"), out);
                if (node.contains("right")) collect(node.at("right"), out);
            };
        std::vector<nlohmann::json> references;
        for (const auto& expression : source.projections) collect(expression, references);
        require(references.size() == 1, "exactly one outer reference recorded");
        require(references.front().value("binding", std::uint32_t{0}) != 0, "the outer reference carries identity");
        require(references.front().value("correlationDepth", std::size_t{0}) == 1, "it crosses exactly one scope");
        require(references.front().value("name", std::string{}) == "id", "the referenced column is named");
    }

    {   // 回归保护：身份缺失时（旧计划文档）优化结果必须与改造前一致。
        // 做法是把同一个计划的 binding/relation 抹掉再优化，两条路径的输出
        // 除身份字段外应逐字节相同。
        const auto strip = [](nlohmann::json document) {
            std::function<void(nlohmann::json&)> scrub = [&](nlohmann::json& node) {
                if (node.is_object()) {
                    node.erase("binding");
                    node.erase("relation");
                    for (auto& item : node.items()) scrub(item.value());
                } else if (node.is_array()) {
                    for (auto& item : node) scrub(item);
                }
            };
            scrub(document);
            return document;
        };
        for (const char* sql : {
                 "SELECT a.x, b.y FROM a JOIN b ON a.id = b.id WHERE b.y > 10;",
                 "SELECT a.x FROM a JOIN b ON a.id = b.id WHERE a.x > 1 AND b.y < 9;",
                 "SELECT x FROM a WHERE id > 1;",
                 "SELECT note FROM a WHERE id > 1 AND x < 4;",
                 "SELECT p.x FROM a AS p JOIN a AS q ON p.id = q.id WHERE q.x > 10;"}) {
            const auto source = plan(catalog, sql);
            const auto withIdentity = optimizer::optimize({source});
            const auto legacy = sql::deserializePlans(strip(sql::serializePlans({source})));
            const auto withoutIdentity = optimizer::optimize(legacy);
            require(strip(sql::serializePlans(withIdentity.plans)) ==
                        strip(sql::serializePlans(withoutIdentity.plans)),
                    "identity-driven and slot-driven optimization agree");
        }
    }

    std::cout << checks << " optimizer binding checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "optimizer binding contract failed: " << error.what() << "\n";
    return 1;
}
}
