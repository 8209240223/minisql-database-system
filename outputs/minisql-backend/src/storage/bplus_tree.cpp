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
// 比较两个单元格值，返回负数、零或正数。
    if (std::holds_alternative<std::monostate>(left) || std::holds_alternative<std::monostate>(right)) {
    // 只要有一边是空值就进入空值规则。
        if (std::holds_alternative<std::monostate>(left) && std::holds_alternative<std::monostate>(right)) return 0;
        // 两侧都空视为相等。
        return std::holds_alternative<std::monostate>(left) ? -1 : 1;
        // 空值排在前面，这与 SQL 的 NULLS FIRST 约定一致。
    }
    const auto number = [](const Value& value) -> std::optional<long double> {
    // 把数值型统一提升成 long double 进行比较，避免 int 与 float 直接比较的精度问题。
        if (std::holds_alternative<std::int32_t>(value)) return static_cast<long double>(std::get<std::int32_t>(value));
        // INT 提升。
        if (std::holds_alternative<std::int64_t>(value)) return static_cast<long double>(std::get<std::int64_t>(value));
        // BIGINT 提升。
        if (std::holds_alternative<double>(value)) return static_cast<long double>(std::get<double>(value));
        // FLOAT 提升。
        return std::nullopt;
        // 非数值返回空。
    };
    const auto numericLeft = number(left), numericRight = number(right);
    // 两侧都尝试转成数值。
    if (numericLeft && numericRight) return *numericLeft < *numericRight ? -1 : *numericLeft > *numericRight ? 1 : 0;
    // 都是数值时直接比大小。
    if (std::holds_alternative<bool>(left) && std::holds_alternative<bool>(right)) {
    // 两侧都是布尔。
        const auto leftBool = std::get<bool>(left), rightBool = std::get<bool>(right);
        // 取值。
        return leftBool == rightBool ? 0 : leftBool ? 1 : -1;
        // 假小于真。
    }
    if (std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right)) {
    // 两侧都是字符串。
        const auto& leftText = std::get<std::string>(left);
        // 左值。
        const auto& rightText = std::get<std::string>(right);
        // 右值。
        return leftText < rightText ? -1 : leftText > rightText ? 1 : 0;
        // 按字典序比较。
    }
    return static_cast<int>(left.index()) < static_cast<int>(right.index()) ? -1 : 1;
    // 类型不同时按 variant 的类型序号给出确定顺序，保证比较是全序。
}
}
nlohmann::json valueJson(const Value& value) {
// 把索引键里的一个值序列化成带类型标签的 JSON。
    if (std::holds_alternative<std::monostate>(value)) return {{"type", "null"}};
    // 空值只写类型。
    if (std::holds_alternative<std::int32_t>(value)) return {{"type", "int"}, {"value", std::get<std::int32_t>(value)}};
    // INT 写类型与数值。
    if (std::holds_alternative<std::int64_t>(value)) return {{"type", "bigint"}, {"value", std::get<std::int64_t>(value)}};
    // BIGINT。
    if (std::holds_alternative<double>(value)) return {{"type", "float"}, {"value", std::get<double>(value)}};
    // FLOAT。
    if (std::holds_alternative<bool>(value)) return {{"type", "bool"}, {"value", std::get<bool>(value)}};
    // BOOL。
    return {{"type", "string"}, {"value", std::get<std::string>(value)}};
    // 其余按字符串处理。
}
Value valueFromJson(const nlohmann::json& value) {
// 反向还原：按类型标签把 JSON 变回 Value。
    const auto type = value.at("type").get<std::string>();
    // 读类型标签。
    if (type == "null") return std::monostate{};
    // 空值。
    if (type == "int") return value.at("value").get<std::int32_t>();
    // INT。
    if (type == "bigint") return value.at("value").get<std::int64_t>();
    // BIGINT。
    if (type == "float") return value.at("value").get<double>();
    // FLOAT。
    if (type == "bool") return value.at("value").get<bool>();
    // BOOL。
    if (type == "string") return value.at("value").get<std::string>();
    // 字符串。
    throw MiniSqlError(ErrorCode::Storage, "Invalid index key value type");
    // 其余标签说明快照有问题。
}
nlohmann::json rowRefJson(const RowRef& row) {
// 行引用序列化：页号、页代数、槽号、槽代数四项都要保存。
    return {{"pageId", row.page.id}, {"pageGeneration", row.page.generation},
        {"slot", row.slot.slot}, {"slotGeneration", row.slot.generation}};
}
RowRef rowRefFromJson(const nlohmann::json& value) {
// 行引用反序列化。
    return {{value.at("pageId").get<PageId>(), value.at("pageGeneration").get<std::uint64_t>()},
        {value.at("slot").get<std::uint16_t>(), value.at("slotGeneration").get<std::uint64_t>()}};
}
int IndexKey::compare(const IndexKey& left, const IndexKey& right) {
// 复合键比较：从左到右逐列比，第一处不同就决定顺序。
    const auto count = std::min(left.values.size(), right.values.size());
    // 只比较两边都有的列。
    for (std::size_t index = 0; index < count; ++index) {
    // 逐列比较。
        const auto order = compareScalar(left.values[index], right.values[index]);
        // 比这一列。
        if (order != 0) return order;
        // 不同就返回结果。
    }
    return left.values.size() < right.values.size() ? -1 : left.values.size() > right.values.size() ? 1 : 0;
    // 前缀相同则短键在前。
}
BPlusTree::BPlusTree(std::size_t maxKeys, bool unique) : maxKeys_(maxKeys), unique_(unique) {
// 构造：校验参数并建立一个空叶子作为根。
    if (maxKeys < 3 || maxKeys > 4096) throw MiniSqlError(ErrorCode::InvalidArgument, "B+ tree maxKeys must be between 3 and 4096");
    // 键数上限过小会让树无法维持性质，过大则退化成线性查找。
    root_ = std::make_unique<Node>();
    // 初始根就是一个空叶子。
}
void BPlusTree::reset() {
    root_ = std::make_unique<Node>();
    size_ = 0;
}
bool BPlusTree::insert(IndexKey key, RowRef row) {
// 插入入口：处理唯一性检查与根分裂。
    if (unique_ && !search(key).empty()) return false;
    // 唯一索引遇到已存在的键直接拒绝，由调用方转成业务错误。
    auto split = insert(*root_, std::move(key), row);
    // 递归插入。
    if (!split) return true;
    // 没有分裂说明树高不变，插入完成。
    auto root = std::make_unique<Node>();
    // 根分裂时要新建根。
    root->leaf = false;
    // 新根是内部节点。
    root->keys.push_back(split->key);
    // 放入被提升的键。
    root->children.push_back(std::move(root_));
    // 原来的根成为第一个子节点。
    root->children.push_back(std::move(split->right));
    // 分裂出的右半成为第二个子节点。
    root_ = std::move(root);
    // 树高加一。
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
    // 插入成功。
}
std::optional<BPlusTree::Split> BPlusTree::insert(Node& node, IndexKey key, RowRef row) {
// 递归插入：叶子直接插，内部节点先下钻再把分裂结果插回自己。
    if (node.leaf) {
    // 叶子分支。
        const auto position = std::lower_bound(node.keys.begin(), node.keys.end(), key,
            [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
            // 找到第一个不小于新键的位置，保证相等键插在前面。
        const auto index = static_cast<std::size_t>(position - node.keys.begin());
        // 转成下标。
        node.keys.insert(position, key);
        // 插入键。
        node.values.insert(node.values.begin() + static_cast<std::ptrdiff_t>(index), row);
        // 在相同位置插入对应行，保持键与值一一对应。
        ++size_;
        // 索引项总数加一。
        if (node.keys.size() <= maxKeys_) return std::nullopt;
        // 没超过上限就不用分裂。
        const auto middle = node.keys.size() / 2;
        // 取中间位置作为分裂点。
        auto right = std::make_unique<Node>();
        // 新建右兄弟。
        right->leaf = true;
        // 右兄弟也是叶子。
        right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
        // 后半部分键搬到右兄弟。
        right->values.assign(node.values.begin() + static_cast<std::ptrdiff_t>(middle), node.values.end());
        // 对应的行也搬过去。
        node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
        // 从原节点删除后半部分键。
        node.values.erase(node.values.begin() + static_cast<std::ptrdiff_t>(middle), node.values.end());
        // 删除对应的行。
        right->next = node.next;
        // 右兄弟接上原来的后继。
        node.next = right.get();
        // 原节点指向右兄弟，叶链保持有序。
        return Split{right->keys.front(), std::move(right)};
        // 叶分裂时把右兄弟的第一个键复制到父节点做分隔键。
    }
    auto position = std::upper_bound(node.keys.begin(), node.keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
        // 内部节点用 upper_bound：相等时走右边子节点。
    const auto childIndex = static_cast<std::size_t>(position - node.keys.begin());
    // 得到要下钻的子节点下标。
    auto split = insert(*node.children[childIndex], std::move(key), row);
    // 递归插入。
    if (!split) return std::nullopt;
    // 子节点没分裂，本层也不用动。
    node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(childIndex), split->key);
    // 把提升键插到对应位置。
    node.children.insert(node.children.begin() + static_cast<std::ptrdiff_t>(childIndex + 1), std::move(split->right));
    // 分裂出的右半成为相邻子节点。
    if (node.keys.size() <= maxKeys_) return std::nullopt;
    // 未超限则结束。
    const auto middle = node.keys.size() / 2;
    // 内部节点的分裂点。
    auto right = std::make_unique<Node>();
    // 新建右兄弟。
    right->leaf = false;
    // 它也是内部节点。
    right->keys.assign(node.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), node.keys.end());
    // 中间键右边之后的键搬走，中间键要提升到父节点。
    right->children.reserve(node.children.size() - middle - 1);
    // 预留子指针空间。
    for (std::size_t index = middle + 1; index < node.children.size(); ++index) right->children.push_back(std::move(node.children[index]));
    // 对应的子指针也搬过去，数量比键多一。
    const auto promoted = node.keys[middle];
    // 取出要提升的中间键。
    node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(middle), node.keys.end());
    // 原节点删除中间键及其右边的键。
    node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(middle + 1), node.children.end());
    // 删除搬走的子指针。
    return Split{promoted, std::move(right)};
    // 返回提升键与右兄弟。
}
const BPlusTree::Node* BPlusTree::findLeaf(const IndexKey& key) const {
// 从根一路下钻到叶子。
    const auto* node = root_.get();
    while (!node->leaf) {
    // 只要不是叶子就继续下钻。
        const auto position = std::upper_bound(node->keys.begin(), node->keys.end(), key,
            [](const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right) < 0; });
            // 找到应该走的子节点。
        node = node->children[static_cast<std::size_t>(position - node->keys.begin())].get();
        // 切换到子节点。
    }
    return node;
    // 返回叶子。
}
std::vector<RowRef> BPlusTree::search(const IndexKey& key) const {
// 等值查找：在叶子里找所有等于该键的项。
    return range(key, true, key, true);
}
void BPlusTree::collect(const Node& node, const std::optional<IndexKey>& lower, bool lowerInclusive,
                        const std::optional<IndexKey>& upper, bool upperInclusive, std::vector<RowRef>& rows) const {
// 递归收集范围内的行。
    if (node.leaf) {
    // 叶子才真正有行数据。
        for (std::size_t index = 0; index < node.keys.size(); ++index) {
        // 逐个键判断。
            if (lower) { const auto order = IndexKey::compare(node.keys[index], *lower); if (order < 0 || (order == 0 && !lowerInclusive)) continue; }
            // 小于下界，或者等于下界但要求开区间，就跳过。
            if (upper) { const auto order = IndexKey::compare(node.keys[index], *upper); if (order > 0 || (order == 0 && !upperInclusive)) continue; }
            // 大于上界，或者等于上界但要求开区间，也跳过。
            rows.push_back(node.values[index]);
            // 通过筛选，收集这一行。
        }
        return;
        // 叶子处理完返回。
    }
    for (const auto& child : node.children) collect(*child, lower, lowerInclusive, upper, upperInclusive, rows);
    // 内部节点递归处理所有子节点；简化实现不做区间剪枝。
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
// 范围查询入口。
    std::vector<RowRef> rows;
    // 结果集合。
    collect(*root_, lower, lowerInclusive, upper, upperInclusive, rows);
    // 从根开始递归收集。
    return rows;
}
std::size_t BPlusTree::height() const {
// 树高：从根走到最左叶子经过的节点数。
    std::size_t result = 1;
    // 至少有一层。
    const auto* node = root_.get();
    while (!node->leaf) { node = node->children.front().get(); ++result; }
    // 每一层加一，直到叶子。
    return result;
    // 返回高度。
}
bool BPlusTree::validate() const { std::size_t leafDepth = 0; return validateNode(*root_, 1, leafDepth); }
// 对外自检入口，用一个引用参数在递归中传递并记录叶子深度。
std::string BPlusTree::dump(const std::string& fingerprint) const {
// 把整棵树序列化成 JSON 文本，节点用数组下标互相引用。
    std::vector<const Node*> order;
    // 节点按先序排列。
    std::unordered_map<const Node*, std::size_t> ids;
    // 节点地址到编号的映射。
    std::function<void(const Node&)> collect = [&](const Node& node) {
    // 递归编号。
        ids.emplace(&node, order.size());order.push_back(&node);
        // 先给自己编号并加入序列。
        for (const auto& child : node.children) collect(*child);
        // 再递归子节点。
    };
    collect(*root_);
    nlohmann::json nodes = nlohmann::json::array();
    // 节点数组。
    for (const auto* node : order) {
    // 逐个序列化。
        nlohmann::json keys = nlohmann::json::array();
        // 键数组。
        for (const auto& key : node->keys) {
        // 每个复合键。
            nlohmann::json values = nlohmann::json::array();
            // 该键的各列值。
            for (const auto& value : key.values) values.push_back(valueJson(value));
            // 逐列序列化。
            keys.push_back(std::move(values));
            // 放入键数组。
        }
        nlohmann::json values = nlohmann::json::array();
        // 行引用数组。
        for (const auto& row : node->values) values.push_back(rowRefJson(row));
        // 叶子节点的行引用。
        nlohmann::json children = nlohmann::json::array();
        // 子节点编号数组。
        for (const auto& child : node->children) children.push_back(ids.at(child.get()));
        // 用编号引用子节点，避免重复展开。
        nodes.push_back({{"leaf", node->leaf}, {"next", node->next ? nlohmann::json(ids.at(node->next)) : nlohmann::json(nullptr)},
            {"keys", keys}, {"values", values}, {"children", children}});
        // 组装一个节点对象，next 同样用编号表示。
    }
    const nlohmann::json document{{"version", 1}, {"fingerprint", fingerprint}, {"maxKeys", maxKeys_},
        {"unique", unique_}, {"size", size_}, {"root", 0}, {"nodes", nodes}};
    // 顶层文档包含版本、指纹、节点参数与全部节点。
    return document.dump();
    // 返回 JSON 文本。
}
void BPlusTree::save(const std::filesystem::path& path, const std::string& fingerprint) const {
// 保存快照：先写临时文件再原子改名，避免写到一半被读到。
    const auto document = dump(fingerprint);
    // 生成 JSON。
    // 不要用 path.string() + ".tmp"：Windows 上会把路径窄转换成本地 ANSI，
    // 含非 ASCII 的路径会被改写。直接做路径拼接保留本地编码。
    auto temporary = path;
    temporary += ".tmp";
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc);if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write B+ tree snapshot");output << document;if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write B+ tree snapshot"); }
    // 单行作用域：打开、写入、检查、关闭。
    std::filesystem::rename(temporary, path);
    // 原子替换正式快照。
}
void BPlusTree::restore(const std::string& bytes, const std::string& expectedFingerprint) {
// 从 JSON 文本恢复整棵树。
    nlohmann::json document;
    try { document = nlohmann::json::parse(bytes); }
    // 解析字符串。
    catch (const std::exception&) { throw MiniSqlError(ErrorCode::Storage, "B+ tree page snapshot invalid JSON"); }
    // 语法错误说明快照不可用。
    if (document.at("version") != 1 || document.at("fingerprint") != expectedFingerprint ||
        document.at("maxKeys") != maxKeys_ || document.at("unique") != unique_) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot fingerprint mismatch");
        // 版本、指纹、节点参数必须与当前索引一致，否则拒绝加载。
    std::vector<std::unique_ptr<Node>> nodes;
    // 先建好所有节点，再连父子关系，因为子引用用的是编号。
    for (const auto& encoded : document.at("nodes")) {
    // 逐个节点。
        auto node = std::make_unique<Node>();
        // 新建节点。
        node->leaf = encoded.at("leaf").get<bool>();
        // 还原叶子标志。
        for (const auto& key : encoded.at("keys")) {
        // 还原键。
            IndexKey value;
            // 复合键。
            for (const auto& item : key) value.values.push_back(valueFromJson(item));
            // 逐列还原。
            node->keys.push_back(std::move(value));
            // 放入键列表。
        }
        for (const auto& row : encoded.at("values")) node->values.push_back(rowRefFromJson(row));
        // 还原行引用。
        nodes.push_back(std::move(node));
        // 收进节点数组。
    }
    for (std::size_t index = nodes.size(); index-- > 0;) {
    // 倒序连接，保证子节点已经就位。
        const auto& encoded = document.at("nodes").at(index);
        // 取原始描述。
        for (const auto& child : encoded.at("children")) {
        // 还原子节点。
            const auto id = child.get<std::size_t>();
            // 子节点编号。
            if (id >= nodes.size()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot child out of range");
            // 越界说明快照损坏。
            nodes[index]->children.push_back(std::move(nodes[id]));
            // 接管子节点的所有权。
        }
        if (!encoded.at("next").is_null()) {
        // 还原叶子链表指针。
            const auto id = encoded.at("next").get<std::size_t>();
            // 后继编号。
            if (id >= nodes.size()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot next out of range");
            // 越界检查。
            nodes[index]->next = nodes[id].get();
            // 记录原始指针，所有权仍由 nodes 数组持有。
        }
    }
    const auto root = document.at("root").get<std::size_t>();
    // 根节点编号。
    if (root >= nodes.size() || !nodes[root]) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot root missing");
    // 根必须存在，说明它没有被当作子节点搬走。
    root_ = std::move(nodes[root]);
    // 接管根。
    size_ = document.at("size").get<std::size_t>();
    // 还原索引项总数。
    if (!validate()) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot validation failed");
    // 恢复后必须通过结构自检，否则拒绝使用。
}
void BPlusTree::load(const std::filesystem::path& path, const std::string& expectedFingerprint) {
// 从文件读取快照。
    std::ifstream input(path, std::ios::binary);
    // 打开文件。
    if (!input) throw MiniSqlError(ErrorCode::Storage, "B+ tree snapshot missing");
    // 文件不存在即报错。
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    // 一次性读完。
    restore(bytes, expectedFingerprint);
    // 交给 restore 解析与校验。
}
bool BPlusTree::validateNode(const Node& node, std::size_t depth, std::size_t& leafDepth) const {
// 递归校验 B+ 树的核心不变量。
    if (node.keys.size() > maxKeys_ || (!node.leaf && node.children.size() != node.keys.size() + 1)) return false;
    // 键数不能超上限；内部节点的子指针数必须恰好是键数加一。
    for (std::size_t index = 1; index < node.keys.size(); ++index) {
        const auto order = IndexKey::compare(node.keys[index - 1], node.keys[index]);
        if (order > 0 || (unique_ && order == 0)) return false;
    }
    if (node.leaf) {
    // 叶子分支。
        if (leafDepth == 0) leafDepth = depth;
        // 第一次到达叶子时记录深度。
        return leafDepth == depth;
        // 所有叶子必须深度相同，这是 B+ 树的平衡性要求。
    }
    for (const auto& child : node.children) if (!validateNode(*child, depth + 1, leafDepth)) return false;
    // 递归校验每个子节点。
    return true;
    // 全部通过。
}
}
