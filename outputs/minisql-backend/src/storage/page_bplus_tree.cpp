#include "minisql/storage/page_bplus_tree.hpp"
#include "minisql/common/error.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <string>

namespace minisql::storage {
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }

void putU8(std::vector<std::uint8_t>& out, std::size_t value) { out.push_back(static_cast<std::uint8_t>(value & 0xffu)); }
void putU16(std::vector<std::uint8_t>& out, std::size_t value) {
    const auto v = static_cast<std::uint32_t>(value & 0xffffu);
    for (unsigned i = 0; i < 2; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
}
void putU32(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}
void putU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

std::uint64_t take(const std::vector<std::uint8_t>& record, std::size_t& offset, std::size_t width, const char* what) {
    if (offset > record.size() || width > record.size() - offset) fail(what);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t(record[offset + i]) << (8 * i);
    offset += width;
    return value;
}

int compareKey(const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right); }
bool sameRow(const RowRef& a, const RowRef& b) {
    return a.page.id == b.page.id && a.page.generation == b.page.generation &&
           a.slot.slot == b.slot.slot && a.slot.generation == b.slot.generation;
}
bool samePage(const PageRef& a, const PageRef& b) {
    return a.id == b.id && a.generation == b.generation;
}
}  // namespace

PageBPlusTree::PageBPlusTree(std::shared_ptr<PageFile> file, BufferPool& buffer, std::uint64_t owner,
                             std::size_t maxKeys, bool unique)
    : file_(std::move(file)), buffer_(buffer), owner_(owner), maxKeys_(maxKeys), unique_(unique),
      minKeys_((maxKeys + 1) / 2) {
    if (!file_ || maxKeys < 3 || maxKeys > 4096) fail("PageBPlusTree requires a file and maxKeys in [3,4096]");
}

bool PageBPlusTree::exists() const {
    if (meta_) return true;
    meta_ = findMeta();
    return meta_.has_value();
}

bool PageBPlusTree::create() {
    if (exists()) return false;
    const auto meta = buffer_.allocate(owner_);
    auto guard = buffer_.get(meta);
    guard.insert(metaRecord(maxKeys_, unique_, 0, {kInvalidPageId, 0}));
    meta_ = meta;
    count_ = 0;
    return true;
}

std::vector<std::uint8_t> PageBPlusTree::encodeValue(const Value& value) {
    std::vector<std::uint8_t> out;
    if (std::holds_alternative<std::monostate>(value)) { out.push_back(0x6e); return out; }
    if (std::holds_alternative<std::int32_t>(value)) {
        out.reserve(5); out.push_back(0x69);
        putU32(out, static_cast<std::uint32_t>(std::get<std::int32_t>(value)));
        return out;
    }
    if (std::holds_alternative<std::int64_t>(value)) {
        out.reserve(9); out.push_back(0x6c);
        putU64(out, static_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        return out;
    }
    if (std::holds_alternative<double>(value)) {
        out.reserve(9); out.push_back(0x64);
        putU64(out, std::bit_cast<std::uint64_t>(std::get<double>(value)));
        return out;
    }
    if (std::holds_alternative<bool>(value)) { out.push_back(0x62); out.push_back(std::get<bool>(value) ? 1u : 0u); return out; }
    const auto& text = std::get<std::string>(value);
    out.reserve(5 + text.size()); out.push_back(0x73);
    putU32(out, text.size());
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

Value PageBPlusTree::decodeValue(const std::vector<std::uint8_t>& record, std::size_t& offset) {
    if (offset >= record.size()) fail("Index page record truncated");
    const auto type = record[offset++];
    if (type == 0x6e) return std::monostate{};
    if (type == 0x69) return static_cast<std::int32_t>(static_cast<std::uint32_t>(take(record, offset, 4, "Index value truncated")));
    if (type == 0x6c) return static_cast<std::int64_t>(take(record, offset, 8, "Index value truncated"));
    if (type == 0x64) return std::bit_cast<double>(take(record, offset, 8, "Index value truncated"));
    if (type == 0x62) {
        if (offset >= record.size()) fail("Index value truncated");
        const auto byte = record[offset++];
        return byte != 0;
    }
    if (type == 0x73) {
        const auto length = static_cast<std::size_t>(take(record, offset, 4, "Index value truncated"));
        if (offset > record.size() || length > record.size() - offset) fail("Index string truncated");
        std::string text(record.begin() + static_cast<std::ptrdiff_t>(offset),
                         record.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
        return text;
    }
    fail("Unknown index value type");
}

std::vector<std::uint8_t> PageBPlusTree::metaRecord(std::size_t maxKeys, bool unique, std::size_t size, PageRef root) {
    std::vector<std::uint8_t> out;
    out.reserve(34);
    out.push_back(IndexMeta); out.push_back(1);
    putU16(out, maxKeys);
    putU32(out, unique ? 1u : 0u);
    putU64(out, size);
    putU64(out, root.id);
    putU64(out, root.generation);
    return out;
}

bool PageBPlusTree::parseMeta(const std::vector<std::uint8_t>& record, std::size_t& maxKeys, bool& unique,
                              std::size_t& size, PageRef& root) {
    if (record.size() < 32 || record[0] != IndexMeta || record[1] != 1) return false;
    std::size_t offset = 2;
    maxKeys = static_cast<std::size_t>(take(record, offset, 2, "Meta truncated"));
    unique = take(record, offset, 4, "Meta truncated") != 0;
    size = static_cast<std::size_t>(take(record, offset, 8, "Meta truncated"));
    root.id = take(record, offset, 8, "Meta truncated");
    root.generation = take(record, offset, 8, "Meta truncated");
    return true;
}

std::optional<PageRef> PageBPlusTree::findMeta() const {
    for (const auto& ref : file_->pagesFor(owner_)) {
        auto guard = buffer_.get(ref);
        const auto slots = guard.page().liveSlots();
        if (slots.size() != 1) continue;
        const auto record = guard.page().read(slots.front());
        if (!record.empty() && record[0] == IndexMeta) return ref;
    }
    return std::nullopt;
}

PageRef PageBPlusTree::requireMeta(PageRef& root, std::size_t& size) const {
    if (!meta_) meta_ = findMeta();
    if (!meta_) fail("Index meta page missing");
    auto guard = buffer_.get(*meta_);
    const auto slots = guard.page().liveSlots();
    if (slots.size() != 1) fail("Corrupt index meta page");
    const auto record = guard.page().read(slots.front());
    std::size_t maxKeys;
    bool unique;
    if (!parseMeta(record, maxKeys, unique, size, root)) fail("Corrupt index meta page");
    if (maxKeys != maxKeys_ || unique != unique_) fail("Index meta format mismatch");
    return *meta_;
}

void PageBPlusTree::writeMeta(PageRef root, std::size_t size) {
    if (!meta_) meta_ = findMeta();
    if (!meta_) fail("Index meta page missing");
    auto guard = buffer_.get(*meta_);
    const auto slots = guard.page().liveSlots();
    if (slots.size() != 1) fail("Corrupt index meta page");
    guard.erase(slots.front());
    count_ = size;
    guard.insert(metaRecord(maxKeys_, unique_, size, root));
}

PageBPlusTree::Node PageBPlusTree::readNode(PageRef ref) const {
    auto guard = buffer_.get(ref);
    const auto slots = guard.page().liveSlots();
    if (slots.size() != 1) fail("Index node page must hold exactly one record");
    return nodeView(guard.page().read(slots.front()));
}

void PageBPlusTree::replaceNode(PageRef ref, const Node& node) {
    auto guard = buffer_.get(ref);
    const auto slots = guard.page().liveSlots();
    if (!slots.empty()) guard.erase(slots.front());
    guard.insert(nodeRecord(node));
}

std::vector<std::uint8_t> PageBPlusTree::nodeRecord(const Node& node) {
    std::vector<std::uint8_t> out;
    out.reserve(64 + node.keys.size() * 24);
    out.push_back(node.leaf ? IndexLeaf : IndexInternal);
    out.push_back(1);
    putU16(out, node.keys.size());
    putU32(out, node.height);
    putU64(out, node.parentId); putU64(out, node.parentGen);
    putU64(out, node.leftId); putU64(out, node.leftGen);
    putU64(out, node.rightId); putU64(out, node.rightGen);
    for (const auto& key : node.keys) {
        putU8(out, key.values.size());
        for (const auto& value : key.values) {
            const auto encoded = encodeValue(value);
            out.insert(out.end(), encoded.begin(), encoded.end());
        }
    }
    if (node.leaf) {
        for (const auto& ref : node.values) {
            putU64(out, ref.page.id); putU64(out, ref.page.generation);
            putU32(out, ref.slot.slot); putU64(out, ref.slot.generation);
        }
    } else {
        for (const auto& child : node.children) { putU64(out, child.id); putU64(out, child.generation); }
    }
    return out;
}

PageBPlusTree::Node PageBPlusTree::nodeView(std::vector<std::uint8_t> record) {
    if (record.size() < 56) fail("Index node record too short");
    std::size_t offset = 0;
    const auto type = record[offset++];
    const auto version = record[offset++];
    if (version != 1) fail("Unsupported index node format version");
    if (type != IndexInternal && type != IndexLeaf) fail("Unknown index page type");
    Node node;
    node.leaf = type == IndexLeaf;
    const auto keyCount = static_cast<std::size_t>(take(record, offset, 2, "Index node truncated"));
    node.height = static_cast<std::uint32_t>(take(record, offset, 4, "Index node truncated"));
    node.parentId = take(record, offset, 8, "Index node truncated");
    node.parentGen = take(record, offset, 8, "Index node truncated");
    node.leftId = take(record, offset, 8, "Index node truncated");
    node.leftGen = take(record, offset, 8, "Index node truncated");
    node.rightId = take(record, offset, 8, "Index node truncated");
    node.rightGen = take(record, offset, 8, "Index node truncated");
    node.keys.reserve(keyCount);
    for (std::size_t i = 0; i < keyCount; ++i) {
        if (offset >= record.size()) fail("Index node key truncated");
        const auto components = record[offset++];
        IndexKey key;
        key.values.reserve(components);
        for (std::size_t j = 0; j < components; ++j) key.values.push_back(decodeValue(record, offset));
        node.keys.push_back(std::move(key));
    }
    if (node.leaf) {
        node.values.reserve(keyCount);
        for (std::size_t i = 0; i < keyCount; ++i) {
            RowRef ref{{take(record, offset, 8, "Index RowRef truncated"), take(record, offset, 8, "Index RowRef truncated")},
                       {static_cast<std::uint32_t>(take(record, offset, 4, "Index RowRef truncated")), take(record, offset, 8, "Index RowRef truncated")}};
            node.values.push_back(ref);
        }
    } else {
        node.children.reserve(keyCount + 1);
        for (std::size_t i = 0; i < keyCount + 1; ++i) {
            const PageId id = take(record, offset, 8, "Index child truncated");
            const auto generation = take(record, offset, 8, "Index child truncated");
            node.children.push_back({id, generation});
        }
    }
    return node;
}

std::size_t PageBPlusTree::childIndex(const std::vector<IndexKey>& keys, const IndexKey& key) const {
    return static_cast<std::size_t>(std::upper_bound(keys.begin(), keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return compareKey(left, right) < 0; }) - keys.begin());
}
std::size_t PageBPlusTree::lowerBound(const std::vector<IndexKey>& keys, const IndexKey& key) const {
    return static_cast<std::size_t>(std::lower_bound(keys.begin(), keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return compareKey(left, right) < 0; }) - keys.begin());
}

PageRef PageBPlusTree::locateLeaf(PageRef root) const {
    auto current = root;
    for (;;) {
        auto node = readNode(current);
        if (node.leaf) return current;
        current = node.children.front();
    }
}
PageRef PageBPlusTree::liftLeaf(PageRef root, const IndexKey& key) const {
    auto current = root;
    for (;;) {
        auto node = readNode(current);
        if (node.leaf) return current;
        current = node.children[lowerBound(node.keys, key)];
    }
}
IndexKey PageBPlusTree::minimumKey(PageRef node) const {
    for (;;) {
        const auto current = readNode(node);
        if (current.leaf) {
            if (current.keys.empty()) fail("Index child has no minimum key");
            return current.keys.front();
        }
        if (current.children.empty()) fail("Index internal node has no children");
        node = current.children.front();
    }
}

bool PageBPlusTree::insert(IndexKey key, RowRef row) {
    if (!exists()) create();
    if (unique_ && !search(key).empty()) return false;
    PageRef root;
    std::size_t size;
    requireMeta(root, size);
    if (root.id == kInvalidPageId) {
        // 空树首插入：直接建一个叶节点作为根
        auto ref = buffer_.allocate(owner_);
        Node leaf;
        leaf.leaf = true;
        leaf.height = 0;
        leaf.keys = {key};
        leaf.values = {row};
        replaceNode(ref, leaf);
        writeMeta(ref, 1);
        return true;
    }
    std::optional<std::pair<IndexKey, PageRef>> split;
    std::optional<PageRef> rootReplace;
    insertInto(root, key, row, true, split, rootReplace);
    auto newRoot = rootReplace ? *rootReplace : root;
    writeMeta(newRoot, size + 1);
    return true;
}

void PageBPlusTree::insertInto(PageRef node, const IndexKey& key, RowRef row, bool isRoot,
                               std::optional<std::pair<IndexKey, PageRef>>& split,
                               std::optional<PageRef>& rootReplace) {
    auto current = readNode(node);
    if (current.leaf) {
        const auto position = lowerBound(current.keys, key);
        current.keys.insert(current.keys.begin() + static_cast<std::ptrdiff_t>(position), key);
        current.values.insert(current.values.begin() + static_cast<std::ptrdiff_t>(position), row);
        split.reset();
        rootReplace.reset();
        if (current.keys.size() <= maxKeys_) { replaceNode(node, current); return; }
        // Leaf split：左保留 [0, mid)，右为 [mid..)
        const auto middle = current.keys.size() / 2;
        Node right;
        right.leaf = true;
        right.height = current.height;
        right.parentId = current.parentId; right.parentGen = current.parentGen;
        right.leftId = node.id; right.leftGen = node.generation;
        right.rightId = current.rightId; right.rightGen = current.rightGen;
        right.keys.assign(current.keys.begin() + static_cast<std::ptrdiff_t>(middle), current.keys.end());
        right.values.assign(current.values.begin() + static_cast<std::ptrdiff_t>(middle), current.values.end());
        current.keys.resize(middle);
        current.values.resize(middle);
        auto rightRef = buffer_.allocate(owner_);
        // 让原右兄弟的左指针指向新的右叶
        if (current.rightId != 0) {
            mutateNode({current.rightId, current.rightGen}, [&](Node& sibling) {
                sibling.leftId = rightRef.id; sibling.leftGen = rightRef.generation;
            });
        }
        current.rightId = rightRef.id; current.rightGen = rightRef.generation;
        replaceNode(node, current);
        replaceNode(rightRef, right);
        if (!isRoot) {
            split = {{right.keys.front(), rightRef}};
        } else {
            // 根叶分裂 → 新建内节点根，两个叶的父指针都指向新根
            const auto oldRoot = readNode(node);
            Node root;
            root.leaf = false;
            root.height = oldRoot.height + 1;
            root.keys = {right.keys.front()};
            root.children = {node, rightRef};
            const auto rootRef = buffer_.allocate(owner_);
            mutateNode(node, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
            mutateNode(rightRef, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
            replaceNode(rootRef, root);
            rootReplace = rootRef;
        }
        return;
    }
    const auto position = childIndex(current.keys, key);
    const auto child = current.children[position];
    std::optional<std::pair<IndexKey, PageRef>> childSplit;
    std::optional<PageRef> childRootReplace;
    insertInto(child, key, row, false, childSplit, childRootReplace);
    split.reset();
    rootReplace.reset();
    if (!childSplit) return;  // 子树全量接受，无需传播
    const auto& promoted = childSplit->first;
    const auto& rightRef = childSplit->second;
    current.keys.insert(current.keys.begin() + static_cast<std::ptrdiff_t>(position), promoted);
    current.children.insert(current.children.begin() + static_cast<std::ptrdiff_t>(position + 1), rightRef);
    mutateNode(rightRef, [&](Node& rightNode) { rightNode.parentId = node.id; rightNode.parentGen = node.generation; });
    replaceNode(node, current);
    if (current.keys.size() <= maxKeys_) return;
    const auto middle = current.keys.size() / 2;
    const IndexKey promotedKey = current.keys[middle];
    Node right;
    right.leaf = false;
    right.height = current.height;
    right.parentId = current.parentId; right.parentGen = current.parentGen;
    right.keys.assign(current.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), current.keys.end());
    right.children.assign(current.children.begin() + static_cast<std::ptrdiff_t>(middle + 1), current.children.end());
    current.keys.resize(middle);
    current.children.resize(middle + 1);
    auto rightNodeRef = buffer_.allocate(owner_);
    for (const auto& moved : right.children) {
        mutateNode(moved, [&](Node& movedNode) { movedNode.parentId = rightNodeRef.id; movedNode.parentGen = rightNodeRef.generation; });
    }
    replaceNode(node, current);
    replaceNode(rightNodeRef, right);
    if (!isRoot) {
        split = {{promotedKey, rightNodeRef}};
    } else {
        // 根内节点分裂 → 新建内节点根，promotedKey 上提
        const auto oldRootHeight = current.height;
        Node root;
        root.leaf = false;
        root.height = oldRootHeight + 1;
        root.keys = {promotedKey};
        root.children = {node, rightNodeRef};
        const auto rootRef = buffer_.allocate(owner_);
        mutateNode(node, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
        mutateNode(rightNodeRef, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
        replaceNode(rootRef, root);
        rootReplace = rootRef;
    }
}

bool PageBPlusTree::erase(IndexKey key, RowRef row) {
    if (!exists()) return false;
    PageRef root;
    std::size_t size;
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return false;
    std::optional<PageRef> rootReplace;
    bool rootUnderflow = false;
    const bool found = eraseInto(root, key, row, root, rootReplace, rootUnderflow);
    if (!found) return false;
    // 若根内节点只剩单个子节点，则收缩根（高度减一）
    if (!rootReplace) {
        const auto rootNode = readNode(root);
        if (!rootNode.leaf && rootNode.keys.empty() && rootNode.children.size() == 1) {
            const auto child = rootNode.children.front();
            mutateNode(child, [](Node& c) { c.parentId = 0; c.parentGen = 0; });
            buffer_.release(root);
            rootReplace = child;
        }
    }
    writeMeta(rootReplace ? *rootReplace : root, size - 1);
    return true;
}

bool PageBPlusTree::eraseInto(PageRef node, const IndexKey& key, RowRef row, const PageRef& root,
                              std::optional<PageRef>& rootReplace, bool& underflow) {
    auto current = readNode(node);
    if (current.leaf) {
        const auto position = lowerBound(current.keys, key);
        bool erased = false;
        for (std::size_t i = position; i < current.keys.size(); ++i) {
            if (compareKey(current.keys[i], key) != 0) break;
            if (sameRow(current.values[i], row)) {
                current.keys.erase(current.keys.begin() + static_cast<std::ptrdiff_t>(i));
                current.values.erase(current.values.begin() + static_cast<std::ptrdiff_t>(i));
                erased = true;
                break;
            }
        }
        replaceNode(node, current);
        underflow = false;
        if (!erased) return false;
        if (!samePage(node, root)) underflow = current.keys.size() < minKeys_;
        return true;
    }
    const auto first = lowerBound(current.keys, key);
    const auto upper = static_cast<std::size_t>(std::upper_bound(current.keys.begin(), current.keys.end(), key,
        [](const IndexKey& value, const IndexKey& separator) { return compareKey(value, separator) < 0; }) - current.keys.begin());
    for (std::size_t position = first; position <= upper && position < current.children.size(); ++position) {
        const auto child = current.children[position];
        bool childUnderflow = false;
        if (!eraseInto(child, key, row, root, rootReplace, childUnderflow)) continue;
        if (childUnderflow) rebalanceChild(current, position, child, samePage(node, root));
        for (std::size_t index = 0; index < current.keys.size(); ++index)
            current.keys[index] = minimumKey(current.children[index + 1]);
        replaceNode(node, current);
        underflow = !samePage(node, root) && current.keys.size() < minKeys_;
        return true;
    }
    underflow = false;
    return false;
}

bool PageBPlusTree::rebalanceChild(Node& n, std::size_t childIdx, PageRef childRef, bool nodeIsRoot) {
    auto child = readNode(childRef);
    const std::size_t min = minKeys_;
    // 1) 向右兄弟借：右兄弟首条目移入本节点末尾
    if (childIdx + 1 < n.children.size()) {
        const auto rightRef = n.children[childIdx + 1];
        auto right = readNode(rightRef);
        if (right.keys.size() > min) {
            if (child.leaf) {
                child.keys.push_back(right.keys.front());
                child.values.push_back(right.values.front());
                right.keys.erase(right.keys.begin());
                right.values.erase(right.values.begin());
                n.keys[childIdx] = right.keys.front();
            } else {
                child.children.push_back(right.children.front());
                child.keys.push_back(n.keys[childIdx]);
                mutateNode(right.children.front(), [&](Node& moved) {
                    moved.parentId = childRef.id; moved.parentGen = childRef.generation;
                });
                n.keys[childIdx] = right.keys.front();
                right.keys.erase(right.keys.begin());
                right.children.erase(right.children.begin());
            }
            replaceNode(childRef, child);
            replaceNode(rightRef, right);
            return !nodeIsRoot && n.keys.size() < min;
        }
    }
    // 2) 向左兄弟借：左兄弟末尾条目移入本节点开头
    if (childIdx > 0) {
        const auto leftRef = n.children[childIdx - 1];
        auto left = readNode(leftRef);
        if (left.keys.size() > min) {
            if (child.leaf) {
                child.keys.insert(child.keys.begin(), left.keys.back());
                child.values.insert(child.values.begin(), left.values.back());
                left.keys.pop_back();
                left.values.pop_back();
                n.keys[childIdx - 1] = child.keys.front();
            } else {
                child.keys.insert(child.keys.begin(), n.keys[childIdx - 1]);
                child.children.insert(child.children.begin(), left.children.back());
                mutateNode(left.children.back(), [&](Node& moved) {
                    moved.parentId = childRef.id; moved.parentGen = childRef.generation;
                });
                n.keys[childIdx - 1] = left.keys.back();
                left.keys.pop_back();
                left.children.pop_back();
            }
            replaceNode(childRef, child);
            replaceNode(leftRef, left);
            return !nodeIsRoot && n.keys.size() < min;
        }
    }
    // 3) 无法借位 → 合并。优先并入左兄弟，否则并入右兄弟。
    const bool intoLeft = childIdx > 0;
    const auto keepRef = intoLeft ? n.children[childIdx - 1] : childRef;
    const auto dropRef = intoLeft ? childRef : n.children[childIdx + 1];
    auto keep = readNode(keepRef);
    auto drop = readNode(dropRef);
    const auto pivot = intoLeft ? n.keys[childIdx - 1] : n.keys[childIdx];
    if (keep.leaf) {
        keep.keys.insert(keep.keys.end(), drop.keys.begin(), drop.keys.end());
        keep.values.insert(keep.values.end(), drop.values.begin(), drop.values.end());
        keep.rightId = drop.rightId; keep.rightGen = drop.rightGen;
    } else {
        keep.keys.push_back(pivot);
        keep.keys.insert(keep.keys.end(), drop.keys.begin(), drop.keys.end());
        keep.children.insert(keep.children.end(), drop.children.begin(), drop.children.end());
        for (const auto& moved : drop.children) {
            mutateNode(moved, [&](Node& m) { m.parentId = keepRef.id; m.parentGen = keepRef.generation; });
        }
        keep.rightId = drop.rightId; keep.rightGen = drop.rightGen;
    }
    // 重连叶链：被删页右侧的兄弟左指改为 keep
    if (drop.rightId != 0) {
        mutateNode({drop.rightId, drop.rightGen}, [&](Node& r) { r.leftId = keepRef.id; r.leftGen = keepRef.generation; });
    }
    replaceNode(keepRef, keep);
    buffer_.release(dropRef);
    if (intoLeft) {
        n.keys.erase(n.keys.begin() + static_cast<std::ptrdiff_t>(childIdx - 1));
        n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(childIdx));
    } else {
        n.keys.erase(n.keys.begin() + static_cast<std::ptrdiff_t>(childIdx));
        n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(childIdx + 1));
    }
    return !nodeIsRoot && n.keys.size() < min;
}

std::vector<RowRef> PageBPlusTree::search(const IndexKey& key) const {
    return range(key, true, key, true);
}

std::vector<RowRef> PageBPlusTree::range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                                         const std::optional<IndexKey>& upper, bool upperInclusive) const {
    std::vector<RowRef> rows;
    if (!const_cast<PageBPlusTree*>(this)->exists()) return rows;
    PageRef root;
    std::size_t size;
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return rows;
    auto current = lower ? liftLeaf(root, *lower) : locateLeaf(root);
    std::size_t guard = 0;
    while (current.id != 0) {
        auto node = const_cast<PageBPlusTree*>(this)->readNode(current);
        bool stop = false;
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
            if (lower) {
                const auto order = compareKey(node.keys[i], *lower);
                if (order < 0 || (order == 0 && !lowerInclusive)) continue;
            }
            if (upper) {
                const auto order = compareKey(node.keys[i], *upper);
                if (order > 0 || (order == 0 && !upperInclusive)) { stop = true; break; }
            }
            rows.push_back(node.values[i]);
        }
        if (stop) break;
        if (++guard > file_->pagesFor(owner_).size() + 1u) fail("Index leaf chain loop");
        current = {node.rightId, node.rightGen};
    }
    return rows;
}

std::size_t PageBPlusTree::size() const {
    PageRef root;
    std::size_t size = 0;
    if (const_cast<PageBPlusTree*>(this)->exists()) requireMeta(root, size);
    return size;
}
std::size_t PageBPlusTree::height() const {
    PageRef root;
    std::size_t size;
    if (!const_cast<PageBPlusTree*>(this)->exists()) return 0;
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return 0;
    const auto rootNode = const_cast<PageBPlusTree*>(this)->readNode(root);
    return static_cast<std::size_t>(rootNode.height) + 1;
}
std::size_t PageBPlusTree::pageCount() const { return file_->pagesFor(owner_).size(); }
bool PageBPlusTree::validate() const {
    if (!exists()) return true;
    PageRef root;
    std::size_t size = 0;
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return size == 0;
    std::size_t leafDepth = 0;
    if (!validateNode(root, 1, std::nullopt, std::nullopt, leafDepth)) return false;
    // 遍历叶链统计行数，校验元数据 size 与叶链完整性
    std::size_t counted = 0;
    auto leaf = locateLeaf(root);
    std::size_t guard = file_->pagesFor(owner_).size() + 2u;
    while (leaf.id != 0) {
        const auto node = readNode(leaf);
        if (!node.leaf) return false;
        counted += node.keys.size();
        leaf = {node.rightId, node.rightGen};
        if (--guard == 0) return false;
    }
    return counted == size;
}
IndexInspect PageBPlusTree::inspect() const {
    IndexInspect result;
    const auto* self = this;
    if (!const_cast<PageBPlusTree*>(self)->exists()) return result;
    PageRef root;
    std::size_t size;
    requireMeta(root, size);
    result.present = true;
    if (meta_) result.meta = *meta_;
    result.root = root;
    if (root.id == kInvalidPageId) { result.rootReachable = true; return result; }

    std::vector<PageRef> leaves;
    bool reachable = true;
    bool parentsValid = true;
    std::size_t visited = 0;
    std::function<void(PageRef)> visit = [&](PageRef ref) {
        if (++visited > maxKeys_ * 4 + 512) { reachable = false; return; }
        const auto node = readNode(ref);
        IndexPageInfo info;
        info.page = ref;
        info.leaf = node.leaf;
        info.height = node.height;
        info.keyCount = node.keys.size();
        info.parent = {node.parentId, node.parentGen};
        info.left = {node.leftId, node.leftGen};
        info.right = {node.rightId, node.rightGen};
        result.pages.push_back(info);
        if (node.leaf) { leaves.push_back(ref); return; }
        for (const auto& child : node.children) {
            const auto childNode = readNode(child);
            if (childNode.parentId != ref.id || childNode.parentGen != ref.generation) parentsValid = false;
            visit(child);
        }
    };
    visit(root);
    result.nodeCount = result.pages.size();
    result.leafCount = leaves.size();
    result.parentLinksValid = parentsValid;
    result.rootReachable = reachable;
    result.height = result.pages.empty() ? 0 : static_cast<std::size_t>(readNode(root).height) + 1;

    // 叶链：从最左叶沿 right 指针顺数，校验右指成链、左右指互成对、且首尾闭合
    bool chainOk = true;
    if (result.height > 0) {
        const auto firstLeef = locateLeaf(root);
        const auto firstNode = readNode(firstLeef);
        if (firstNode.leftId != 0 || firstNode.leftGen != 0) { chainOk = false; reachable = false; }
        result.rowCount += firstNode.keys.size();
        std::size_t chain = 1;
        result.leafChainLength = 1;
        auto current = firstLeef;
        while (true) {
            const auto node = readNode(current);
            if (node.rightId == 0) break;
            const auto nextRef = PageRef{node.rightId, node.rightGen};
            const auto nextNode = readNode(nextRef);
            if (nextNode.leftId != current.id || nextNode.leftGen != current.generation) chainOk = false;
            current = nextRef;
            result.rowCount += nextNode.keys.size();
            ++chain;
            result.leafChainLength = chain;
            if (chain > result.nodeCount + 2) { reachable = false; chainOk = false; break; }
        }
    }
    result.leafChainLinked = chainOk && reachable;
    result.rootReachable = reachable;
    if (!result.leafChainLinked) result.problems.push_back("leaf chain left/right pointers broken");
    if (!parentsValid) result.problems.push_back("child parent pointers mismatch");
    if (!reachable) result.problems.push_back("tree traversal did not terminate");
    return result;
}
bool PageBPlusTree::validateNode(PageRef ref, std::uint32_t depth, std::optional<const IndexKey*> left,
                                 std::optional<const IndexKey*> right, std::size_t& leafDepth) const {
    const auto node = readNode(ref);
    if (node.keys.size() > maxKeys_) return false;
    for (std::size_t i = 1; i < node.keys.size(); ++i) {
        const auto order = compareKey(node.keys[i - 1], node.keys[i]);
        if (order > 0 || (unique_ && order == 0)) return false;
    }
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
        if (left && compareKey(node.keys[i], **left) < 0) return false;
        if (right && compareKey(node.keys[i], **right) > 0) return false;
    }
    if (node.leaf) {
        if (leafDepth == 0) leafDepth = depth;
        return leafDepth == depth;
    }
    if (node.children.size() != node.keys.size() + 1) return false;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        std::optional<const IndexKey*> lo = i > 0 ? std::optional<const IndexKey*>(&node.keys[i - 1]) : left;
        std::optional<const IndexKey*> hi = i < node.keys.size() ? std::optional<const IndexKey*>(&node.keys[i]) : right;
        if (!validateNode(node.children[i], depth + 1, lo, hi, leafDepth)) return false;
    }
    return true;
}
}  // namespace minisql::storage
