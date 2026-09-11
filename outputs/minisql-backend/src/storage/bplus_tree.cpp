#include "minisql/storage/bplus_tree.hpp"
#include "minisql/common/error.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace minisql::storage {
namespace {
int compareScalar(const Value& left, const Value& right) {
    if (std::holds_alternative<std::monostate>(left) || std::holds_alternative<std::monostate>(right)) {
        if (std::holds_alternative<std::monostate>(left) && std::holds_alternative<std::monostate>(right)) return 0;
        return std::holds_alternative<std::monostate>(left) ? -1 : 1;
    }
    const auto number = [](const Value& value) -> std::optional<long double> {
        if (std::holds_alternative<std::int32_t>(value)) return static_cast<long double>(std::get<std::int32_t>(value));
        if (std::holds_alternative<std::int64_t>(value)) return static_cast<long double>(std::get<std::int64_t>(value));
        if (std::holds_alternative<double>(value)) return static_cast<long double>(std::get<double>(value));
        return std::nullopt;
    };
    const auto numericLeft = number(left), numericRight = number(right);
    if (numericLeft && numericRight) return *numericLeft < *numericRight ? -1 : *numericLeft > *numericRight ? 1 : 0;
    if (std::holds_alternative<bool>(left) && std::holds_alternative<bool>(right)) {
        const auto leftBool = std::get<bool>(left), rightBool = std::get<bool>(right);
        return leftBool == rightBool ? 0 : leftBool ? 1 : -1;
    }
    if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
        const auto& leftText = std::get<std::string>(left);
        const auto& rightText = std::get<std::string>(right);
        return leftText < rightText ? -1 : leftText > rightText ? 1 : 0;
    }
    return static_cast<int>(left.index()) < static_cast<int>(right.index()) ? -1 : 1;
}
}
nlohmann::json valueJson(const Value& value) {
    if (std::holds_alternative<std::monostate>(value)) return {{"type", "null"}};
    if (std::holds_alternative<std::int32_t>(value)) return {{"type", "int"}, {"value", std::get<std::int32_t>(value)}};
    if (std::holds_alternative<std::int64_t>(value)) return {{"type", "bigint"}, {"value", std::get<std::int64_t>(value)}};
    if (std::holds_alternative<double>(value)) return {{"type", "float"}, {"value", std::get<double>(value)}};
    if (std::holds_alternative<bool>(value)) return {{"type", "bool"}, {"value", std::get<bool>(value)}};
    return {{"type", "string"}, {"value", std::get<std::string>(value)}};
}
Value valueFromJson(const nlohmann::json& value) {
    const auto type = value.at("type").get<std::string>();
    if (type == "null") return std::monostate{};
    if (type == "int") return value.at("value").get<std::int32_t>();
    if (type == "bigint") return value.at("value").get<std::int64_t>();
    if (type == "float") return value.at("value").get<double>();
    if (type == "bool") return value.at("value").get<bool>();
    if (type == "string") return value.at("value").get<std::string>();
    throw MiniSqlError(ErrorCode::Storage, "Invalid index key value type");
}
nlohmann::json rowRefJson(const RowRef& row) {
    return {{"pageId", row.page.id}, {"pageGeneration", row.page.generation},
        {"slot", row.slot.slot}, {"slotGeneration", row.slot.generation}};
}
RowRef rowRefFromJson(const nlohmann::json& value) {
    return {{value.at("pageId").get<PageId>(), value.at("pageGeneration").get<std::uint64_t>()},
        {value.at("slot").get<std::uint16_t>(), value.at("slotGeneration").get<std::uint64_t>()}};
}
int IndexKey::compare(const IndexKey& left, const IndexKey& right) {
    const auto count = std::min(left.values.size(), right.values.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto order = compareScalar(left.values[index], right.values[index]);
        if (order != 0) return order;
    }
    return left.values.size() < right.values.size() ? -1 : left.values.size() > right.values.size() ? 1 : 0;
}
BPlusTree::BPlusTree(std::size_t maxKeys, bool unique) : maxKeys_(maxKeys), unique_(unique) {
    if (maxKeys < 3 || maxKeys > 4096) throw MiniSqlError(ErrorCode::InvalidArgument, "B+ tree maxKeys must be between 3 and 4096");
    root_ = std::make_unique<Node>();
}
bool BPlusTree::insert(IndexKey key, RowRef row) {
    if (unique_ && !search(key).empty()) return false;
    auto split = insert(*root_, std::move(key), row);
    if (!split) return true;
    auto root = std::make_unique<Node>();
    root->leaf = false;
    root->keys.push_back(split->key);
    root->children.push_back(std::move(root_));
    root->children.push_back(std::move(split->right));
    root_ = std::move(root);
    return true;
}
bool BPlusTree::erase(const IndexKey& key, RowRef row) {
    std::vector<IndexEntry> entries;
    entries.reserve(size_);
    collectEntries(*root_, entries);
    const auto sameRow = [&](const RowRef& candidate) {
        return candidate.page.id == row.page.id && candidate.page.generation == row.page.generation &&
               candidate.slot.slot == row.slot.slot && candidate.slot.generation == row.slot.generation;
    };
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const IndexEntry& entry) {
        return IndexKey::compare(entry.key, key) == 0 && sameRow(entry.row);
    });
    if (found == entries.end()) return false;
    entries.erase(found);
    root_ = std::make_unique<Node>();
    size_ = 0;
    for (auto& entry : entries) {
        if (!insert(std::move(entry.key), entry.row))
            throw MiniSqlError(ErrorCode::Storage, "Cannot restore in-memory B+ tree after erase");
    }
    return true;
}
std::optional<BPlusTree::Split> BPlusTree::insert(Node& node, IndexKey key, RowRef row) {
    if (node.leaf) {
        const auto position = std::lower_bound(node.keys.begin(), node.keys.end(), key,
            [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
        const auto index = static_cast<std::size_t>(position - node.keys.begin());
        node.keys.insert(position, key);
        node.values.insert(node.values.begin() + static_cast<std::ptrdiff_t>(index), row);
        ++size_;
        if (node.keys.size() <= maxKeys_) return std::nullopt;
        const auto middle = node.keys.size() / 2;
        auto right = std::make_unique<Node>();
        right->leaf = true;
        right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
        right->values.assign(node.values.begin() + static_cast<std::ptrdiff_t>(middle), node.values.end());
        node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
        node.values.erase(node.values.begin() + static_cast<std::ptrdiff_t>(middle), node.values.end());
        right->next = node.next;
        node.next = right.get();
        return Split{right->keys.front(), std::move(right)};
    }
    auto position = std::upper_bound(node.keys.begin(), node.keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
    const auto childIndex = static_cast<std::size_t>(position - node.keys.begin());
    auto split = insert(*node.children[childIndex], std::move(key), row);
    if (!split) return std::nullopt;
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(childIndex), split->key);
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(childIndex + 1), std::move(split->right));
    if (node.keys.size() <= maxKeys_) return std::nullopt;
    const auto middle = node.keys.size() / 2;
    auto right = std::make_unique<Node>();
    right->leaf = false;
    right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), node.keys.end());
    right->children.reserve(node.children.size() - middle - 1);
    for (std::size_t index = middle + 1; index < node.children.size(); ++index) right->children.push_back(std::move(node.children[index]));
    const auto promoted = node.keys[middle];
    node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
    node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(middle + 1), node.children.end());
    return Split{promoted, std::move(right)};
}
const BPlusTree::Node* BPlusTree::findLeaf(const IndexKey& key) const {
    const auto* node = root_.get();
    while (!node->leaf) {
        const auto position = std::upper_bound(node->keys.begin(), node->keys.end(), key,
            [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
        node = node->children[static_cast<std::size_t>(position - node->keys.begin())].get();
    }
    return node;
}
std::vector<RowRef> BPlusTree::search(const IndexKey& key) const {
    return range(key, true, key, true);
}
void BPlusTree::collect(const Node& node, const std::optional<IndexKey>& lower, bool lowerInclusive,
                        const std::optional<IndexKey>& upper, bool upperInclusive, std::vector<RowRef>& rows) const {
    if (node.leaf) {
        for (std::size_t index = 0; index < node.keys.size(); ++index) {
            if (lower) { const auto order = IndexKey::compare(node.keys[index], *lower); if (order < 0 || (order == 0 && !lowerInclusive)) continue; }
            if (upper) { const auto order = IndexKey::compare(node.keys[index], *upper); if (order > 0 || (order == 0 && !upperInclusive)) continue; }
            rows.push_back(node.values[index]);
        }
        return;
    }
    for (const auto& child : node.children) collect(*child, lower, lowerInclusive, upper, upperInclusive, rows);
}
void BPlusTree::collectEntries(const Node& node, std::vector<IndexEntry>& entries) const {
    if (node.leaf) {
        for (std::size_t index = 0; index < node.keys.size(); ++index)
            entries.push_back({node.keys[index], node.values[index]});
        return;
    }
    for (const auto& child : node.children) collectEntries(*child, entries);
}
std::vector<RowRef> BPlusTree::range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                                     const std::optional<IndexKey>& upper, bool upperInclusive) const {
    std::vector<RowRef> rows;
    collect(*root_, lower, lowerInclusive, upper, upperInclusive, rows);
    return rows;
}
std::size_t BPlusTree::height() const {
    std::size_t result = 1;
    const auto* node = root_.get();
    while (!node->leaf) { node = node->children.front().get(); ++result; }
    return result;
}
bool BPlusTree::validate() const { std::size_t leafDepth = 0; return validateNode(*root_, 1, leafDepth); }
std::string BPlusTree::dump(const std::string& fingerprint) const {
    std::vector<const Node*> order;
    std::unordered_map<const Node*, std::size_t> ids;
    std::function<void(const Node&)> collect = [&](const Node& node) {
        ids.emplace(&node, order.size());order.push_back(&node);
        for (const auto& child : node.children) collect(*child);
    };
    collect(*root_);
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto* node : order) {
        nlohmann::json keys = nlohmann::json::array();
        for (const auto& key : node->keys) {
            nlohmann::json values = nlohmann::json::array();
            for (const auto& value : key.values) values.push_back(valueJson(value));
            keys.push_back(std::move(values));
        }
        nlohmann::json values = nlohmann::json::array();
        for (const auto& row : node->values) values.push_back(rowRefJson(row));
        nlohmann::json children = nlohmann::json::array();
        for (const auto& child : node->children) children.push_back(ids.at(child.get()));
        nodes.push_back({{"leaf", node->leaf}, {"next", node->next ? nlohmann::json(ids.at(node->next)) : nlohmann::json(nullptr)},
            {"keys", keys}, {"values", values}, {"children", children}});
    }
    const nlohmann::json document{{"version", 1}, {"fingerprint", fingerprint}, {"maxKeys", maxKeys_},
        {"unique", unique_}, {"size", size_}, {"root", 0}, {"nodes", nodes}};
    return document.dump();
}
void BPlusTree::save(const std::filesystem::path& path, const std::string& fingerprint) const {
    const auto document = dump(fingerprint);
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc);if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write B+ tree snapshot");output << document;if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write B+ tree snapshot"); }
    std::filesystem::rename(temporary, path);
}
void BPlusTree::restore(const std::string& bytes, const std::string& expectedFingerprint) {
    nlohmann::json document;
    try { document = nlohmann::json::parse(bytes); }
    catch (const std::exception&) { throw MiniSqlError(ErrorCode::Storage, "B+ tree page snapshot invalid JSON"); }
    if (document.at("version") != 1 || document.at("fingerprint") != expectedFingerprint ||
        document.at("maxKeys") != maxKeys_ || document.at("unique") != unique_) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot fingerprint mismatch");
    std::vector<std::unique_ptr<Node>> nodes;
    for (const auto& encoded : document.at("nodes")) {
        auto node = std::make_unique<Node>();
        node->leaf = encoded.at("leaf").get<bool>();
        for (const auto& key : encoded.at("keys")) {
            IndexKey value;
            for (const auto& item : key) value.values.push_back(valueFromJson(item));
            node->keys.push_back(std::move(value));
        }
        for (const auto& row : encoded.at("values")) node->values.push_back(rowRefFromJson(row));
        nodes.push_back(std::move(node));
    }
    for (std::size_t index = nodes.size(); index-- > 0;) {
        const auto& encoded = document.at("nodes").at(index);
        for (const auto& child : encoded.at("children")) {
            const auto id = child.get<std::size_t>();
            if (id >= nodes.size()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot child out of range");
            nodes[index]->children.push_back(std::move(nodes[id]));
        }
        if (!encoded.at("next").is_null()) {
            const auto id = encoded.at("next").get<std::size_t>();
            if (id >= nodes.size()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot next out of range");
            nodes[index]->next = nodes[id].get();
        }
    }
    const auto root = document.at("root").get<std::size_t>();
    if (root >= nodes.size() || !nodes[root]) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot root missing");
    root_ = std::move(nodes[root]);
    size_ = document.at("size").get<std::size_t>();
    if (!validate()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot validation failed");
}
void BPlusTree::load(const std::filesystem::path& path, const std::string& expectedFingerprint) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot missing");
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    restore(bytes, expectedFingerprint);
}
bool BPlusTree::validateNode(const Node& node, std::size_t depth, std::size_t& leafDepth) const {
    if (node.keys.size() > maxKeys_ || (!node.leaf && node.children.size() != node.keys.size() + 1)) return false;
    for (std::size_t index = 1; index < node.keys.size(); ++index) {
        const auto order = IndexKey::compare(node.keys[index - 1], node.keys[index]);
        if (order > 0 || (unique_ && order == 0)) return false;
    }
    if (node.leaf) {
        if (leafDepth == 0) leafDepth = depth;
        return leafDepth == depth;
    }
    for (const auto& child : node.children) if (!validateNode(*child, depth + 1, leafDepth)) return false;
    return true;
}
}
