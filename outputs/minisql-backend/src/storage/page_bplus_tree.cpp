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
// 统一的存储错误出口。

void putU8(std::vector<std::uint8_t>& out, std::size_t value) { out.push_back(static_cast<std::uint8_t>(value & 0xffu)); }
// 追加 1 字节，只保留最低 8 位。
void putU16(std::vector<std::uint8_t>& out, std::size_t value) {
// 追加 2 字节小端无符号整数。
    const auto v = static_cast<std::uint32_t>(value & 0xffffu);
    // 先截断到 16 位。
    for (unsigned i = 0; i < 2; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xffu));
    // 低字节在前。
}
void putU32(std::vector<std::uint8_t>& out, std::uint64_t value) {
// 追加 4 字节小端无符号整数。
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    // 逐字节输出。
}
void putU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
// 追加 8 字节小端无符号整数。
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    // 逐字节输出。
}

std::uint64_t take(const std::vector<std::uint8_t>& record, std::size_t& offset, std::size_t width, const char* what) {
// 从记录中读取 width 字节的小端整数，并推进游标。
    if (offset > record.size() || width > record.size() - offset) fail(what);
    // 越界说明页内记录被截断，错误文案由调用方给出。
    std::uint64_t value = 0;
    // 累加结果。
    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t(record[offset + i]) << (8 * i);
    // 按小端拼装。
    offset += width;
    // 游标前移。
    return value;
    // 返回读到的值。
}

int compareKey(const IndexKey& left, const IndexKey& right) { return IndexKey::compare(left, right); }
// 索引键比较的转发函数，便于在算法里当比较器用。
bool sameRow(const RowRef& a, const RowRef& b) {
// 判断两个行引用是否指向同一条记录，四个字段都要相等。
    return a.page.id == b.page.id && a.page.generation == b.page.generation &&
           a.slot.slot == b.slot.slot && a.slot.generation == b.slot.generation;
}
bool samePage(const PageRef& a, const PageRef& b) {
// 判断两个页引用是否同一页，页号与代数都要相等。
    return a.id == b.id && a.generation == b.generation;
}
}  // namespace

PageBPlusTree::PageBPlusTree(std::shared_ptr<PageFile> file, BufferPool& buffer, std::uint64_t owner,
                             std::size_t maxKeys, bool unique)
    : file_(std::move(file)), buffer_(buffer), owner_(owner), maxKeys_(maxKeys), unique_(unique),
      minKeys_((maxKeys + 1) / 2) {
    // 保存依赖与参数；minKeys 取上限的一半向上取整，作为下溢判据。
    if (!file_ || maxKeys < 3 || maxKeys > 4096) fail("PageBPlusTree requires a file and maxKeys in [3,4096]");
    // 键数上限太小无法维持 B+ 树性质，太大则单页放不下。
}

bool PageBPlusTree::exists() const {
// 判断索引是否已经建立。
    if (meta_) return true;
    // 已缓存的元页位置可以直接回答。
    meta_ = findMeta();
    // 否则去该索引占用的页里找。
    return meta_.has_value();
    // 找到即为存在。
}

bool PageBPlusTree::create() {
// 建立索引：分配一张元页并写入初始内容。
    if (exists()) return false;
    // 已存在就不重复创建。
    const auto meta = buffer_.allocate(owner_);
    // 分配元页。
    auto guard = buffer_.get(meta);
    // 取到该页。
    guard.insert(metaRecord(maxKeys_, unique_, 0, {kInvalidPageId, 0}));
    // 写入元记录：大小为 0，根为无效页号，表示空树。
    meta_ = meta;
    // 缓存元页位置。
    count_ = 0;
    // 本地计数清零。
    return true;
    // 创建成功。
}

std::vector<std::uint8_t> PageBPlusTree::encodeValue(const Value& value) {
// 把一个键值编码成「类型标记 + 数据」的字节序列。
    std::vector<std::uint8_t> out;
    // 输出缓冲。
    if (std::holds_alternative<std::monostate>(value)) { out.push_back(0x6e); return out; }
    // 0x6e 表示空值，只占一字节，没有数据体。
    if (std::holds_alternative<std::int32_t>(value)) {
    // INT 分支。
        out.reserve(5); out.push_back(0x69);
        // 0x69 标记，随后 4 字节。
        putU32(out, static_cast<std::uint32_t>(std::get<std::int32_t>(value)));
        // 写入位模式，负数也能原样还原。
        return out;
    }
    if (std::holds_alternative<std::int64_t>(value)) {
    // BIGINT 分支。
        out.reserve(9); out.push_back(0x6c);
        // 0x6c 标记，随后 8 字节。
        putU64(out, static_cast<std::uint64_t>(std::get<std::int64_t>(value)));
        // 写位模式。
        return out;
    }
    if (std::holds_alternative<double>(value)) {
    // FLOAT 分支。
        out.reserve(9); out.push_back(0x64);
        // 0x64 标记，随后 8 字节。
        putU64(out, std::bit_cast<std::uint64_t>(std::get<double>(value)));
        // 按 IEEE754 位模式写入。
        return out;
    }
    if (std::holds_alternative<bool>(value)) { out.push_back(0x62); out.push_back(std::get<bool>(value) ? 1u : 0u); return out; }
    // BOOL：0x62 标记加一字节真值。
    const auto& text = std::get<std::string>(value);
    // 其余按字符串处理。
    out.reserve(5 + text.size()); out.push_back(0x73);
    // 0x73 标记，随后 4 字节长度。
    putU32(out, text.size());
    // 写长度。
    out.insert(out.end(), text.begin(), text.end());
    // 写内容本身。
    return out;
    // 返回编码结果。
}

Value PageBPlusTree::decodeValue(const std::vector<std::uint8_t>& record, std::size_t& offset) {
// 从记录里还原一个键值。
    if (offset >= record.size()) fail("Index page record truncated");
    // 连类型标记都没有说明记录被截断。
    const auto type = record[offset++];
    // 读类型标记并推进游标。
    if (type == 0x6e) return std::monostate{};
    // 空值。
    if (type == 0x69) return static_cast<std::int32_t>(static_cast<std::uint32_t>(take(record, offset, 4, "Index value truncated")));
    // INT：读 4 字节再按有符号解释。
    if (type == 0x6c) return static_cast<std::int64_t>(take(record, offset, 8, "Index value truncated"));
    // BIGINT：读 8 字节。
    if (type == 0x64) return std::bit_cast<double>(take(record, offset, 8, "Index value truncated"));
    // FLOAT：读 8 字节位模式再还原成浮点。
    if (type == 0x62) {
    // BOOL 分支。
        if (offset >= record.size()) fail("Index value truncated");
        // 需要一字节真值。
        const auto byte = record[offset++];
        // 读取。
        return byte != 0;
        // 非零为真。
    }
    if (type == 0x73) {
    // 字符串分支。
        const auto length = static_cast<std::size_t>(take(record, offset, 4, "Index value truncated"));
        // 读长度。
        if (offset > record.size() || length > record.size() - offset) fail("Index string truncated");
        // 长度超过剩余字节说明损坏。
        std::string text(record.begin() + static_cast<std::ptrdiff_t>(offset),
                         record.begin() + static_cast<std::ptrdiff_t>(offset + length));
        // 按长度取出内容。
        offset += length;
        // 推进游标。
        return text;
        // 返回字符串。
    }
    fail("Unknown index value type");
    // 未知类型标记属于损坏。
}

std::vector<std::uint8_t> PageBPlusTree::metaRecord(std::size_t maxKeys, bool unique, std::size_t size, PageRef root) {
// 序列化元页内容。
    std::vector<std::uint8_t> out;
    // 输出缓冲。
    out.reserve(34);
    // 预留固定长度。
    out.push_back(IndexMeta); out.push_back(1);
    // 页类型标记与格式版本。
    putU16(out, maxKeys);
    // 节点键数上限。
    putU32(out, unique ? 1u : 0u);
    // 是否唯一索引。
    putU64(out, size);
    // 索引项总数。
    putU64(out, root.id);
    // 根页号。
    putU64(out, root.generation);
    // 根页代数。
    return out;
    // 返回元记录。
}

bool PageBPlusTree::parseMeta(const std::vector<std::uint8_t>& record, std::size_t& maxKeys, bool& unique,
                              std::size_t& size, PageRef& root) {
// 解析元页内容，格式不符时返回 false 而不抛异常。
    if (record.size() < 32 || record[0] != IndexMeta || record[1] != 1) return false;
    // 长度、页类型与版本三项先做检查。
    std::size_t offset = 2;
    // 从标记之后开始读。
    maxKeys = static_cast<std::size_t>(take(record, offset, 2, "Meta truncated"));
    // 读键数上限。
    unique = take(record, offset, 4, "Meta truncated") != 0;
    // 读唯一标志。
    size = static_cast<std::size_t>(take(record, offset, 8, "Meta truncated"));
    // 读索引项总数。
    root.id = take(record, offset, 8, "Meta truncated");
    // 读根页号。
    root.generation = take(record, offset, 8, "Meta truncated");
    // 读根页代数。
    return true;
    // 解析成功。
}

std::optional<PageRef> PageBPlusTree::findMeta() const {
// 在该索引占用的页里寻找元页。
    for (const auto& ref : file_->pagesFor(owner_)) {
    // 遍历归属该索引的所有页。
        auto guard = buffer_.get(ref);
        // 取页。
        const auto slots = guard.page().liveSlots();
        // 元页只应当有一个有效槽。
        if (slots.size() != 1) continue;
        // 槽数不符就不是元页。
        const auto record = guard.page().read(slots.front());
        // 读记录。
        if (!record.empty() && record[0] == IndexMeta) return ref;
        // 首字节是元页标记即认定找到。
    }
    return std::nullopt;
    // 没找到说明索引尚未创建。
}

PageRef PageBPlusTree::requireMeta(PageRef& root, std::size_t& size) const {
// 读取元页，并顺带把根引用与大小通过出参返回。
    if (!meta_) meta_ = findMeta();
    // 先确保知道元页位置。
    if (!meta_) fail("Index meta page missing");
    // 没有元页说明索引不存在。
    auto guard = buffer_.get(*meta_);
    // 取元页。
    const auto slots = guard.page().liveSlots();
    // 取有效槽。
    if (slots.size() != 1) fail("Corrupt index meta page");
    // 元页必须恰好一条记录。
    const auto record = guard.page().read(slots.front());
    // 读记录。
    std::size_t maxKeys;
    // 读出的键数上限。
    bool unique;
    // 读出的唯一标志。
    if (!parseMeta(record, maxKeys, unique, size, root)) fail("Corrupt index meta page");
    // 解析失败即损坏。
    if (maxKeys != maxKeys_ || unique != unique_) fail("Index meta format mismatch");
    // 元页记录的参数必须与当前构造参数一致，否则说明拿错了索引。
    return *meta_;
    // 返回元页引用。
}

void PageBPlusTree::writeMeta(PageRef root, std::size_t size) {
// 更新元页：先删旧记录再插入新记录。
    if (!meta_) meta_ = findMeta();
    // 确保知道元页位置。
    if (!meta_) fail("Index meta page missing");
    // 没有元页无法更新。
    auto guard = buffer_.get(*meta_);
    // 取元页。
    const auto slots = guard.page().liveSlots();
    // 取有效槽。
    if (slots.size() != 1) fail("Corrupt index meta page");
    // 结构检查。
    guard.erase(slots.front());
    // 删除旧元记录。
    count_ = size;
    // 同步本地计数。
    guard.insert(metaRecord(maxKeys_, unique_, size, root));
    // 写入新元记录。
}

PageBPlusTree::Node PageBPlusTree::readNode(PageRef ref) const {
// 读取并解析一个节点页。
    auto guard = buffer_.get(ref);
    // 取页。
    const auto slots = guard.page().liveSlots();
    // 取有效槽。
    if (slots.size() != 1) fail("Index node page must hold exactly one record");
    // 一个节点页只放一条节点记录，槽数不符即损坏。
    return nodeView(guard.page().read(slots.front()));
    // 读出并解析。
}

void PageBPlusTree::replaceNode(PageRef ref, const Node& node) {
// 用新内容整体覆盖节点页。
    auto guard = buffer_.get(ref);
    // 取页。
    const auto slots = guard.page().liveSlots();
    // 取有效槽。
    if (!slots.empty()) guard.erase(slots.front());
    // 有旧记录先删掉；注意页空之后不能立刻释放，因为这里还要重新插入。
    guard.insert(nodeRecord(node));
    // 写入新记录。
}

std::vector<std::uint8_t> PageBPlusTree::nodeRecord(const Node& node) {
// 序列化节点：固定头 + 键 + （叶子）行引用 或 （内部）子页引用。
    std::vector<std::uint8_t> out;
    // 输出缓冲。
    out.reserve(64 + node.keys.size() * 24);
    // 按键数预留，减少扩容。
    out.push_back(node.leaf ? IndexLeaf : IndexInternal);
    // 页类型标记。
    out.push_back(1);
    // 格式版本。
    putU16(out, node.keys.size());
    // 键数量。
    putU32(out, node.height);
    // 树高字段。
    putU64(out, node.parentId); putU64(out, node.parentGen);
    // 父页号与代数。
    putU64(out, node.leftId); putU64(out, node.leftGen);
    // 左兄弟。
    putU64(out, node.rightId); putU64(out, node.rightGen);
    // 右兄弟。
    for (const auto& key : node.keys) {
    // 逐键写入。
        putU8(out, key.values.size());
        // 先写这个键有几列，支持复合键。
        for (const auto& value : key.values) {
        // 逐列写值。
            const auto encoded = encodeValue(value);
            // 编码。
            out.insert(out.end(), encoded.begin(), encoded.end());
            // 追加。
        }
    }
    if (node.leaf) {
    // 叶子节点写行引用。
        for (const auto& ref : node.values) {
        // 每个键一个行引用。
            putU64(out, ref.page.id); putU64(out, ref.page.generation);
            // 页号与页代数。
            putU32(out, ref.slot.slot); putU64(out, ref.slot.generation);
            // 槽号与槽代数。
        }
    } else {
    // 内部节点写子页引用。
        for (const auto& child : node.children) { putU64(out, child.id); putU64(out, child.generation); }
        // 每对页号与代数。
    }
    return out;
    // 返回序列化结果。
}

PageBPlusTree::Node PageBPlusTree::nodeView(std::vector<std::uint8_t> record) {
// 解析节点记录，流程与 nodeRecord 严格对称。
    if (record.size() < 56) fail("Index node record too short");
    // 固定头长度都不够，必然损坏。
    std::size_t offset = 0;
    // 读游标。
    const auto type = record[offset++];
    // 页类型标记。
    const auto version = record[offset++];
    // 格式版本。
    if (version != 1) fail("Unsupported index node format version");
    // 版本不符拒绝解析。
    if (type != IndexInternal && type != IndexLeaf) fail("Unknown index page type");
    // 元页不该走这里。
    Node node;
    // 结果节点。
    node.leaf = type == IndexLeaf;
    // 还原叶子标志。
    const auto keyCount = static_cast<std::size_t>(take(record, offset, 2, "Index node truncated"));
    // 键数量。
    node.height = static_cast<std::uint32_t>(take(record, offset, 4, "Index node truncated"));
    // 树高。
    node.parentId = take(record, offset, 8, "Index node truncated");
    // 父页号。
    node.parentGen = take(record, offset, 8, "Index node truncated");
    // 父页代数。
    node.leftId = take(record, offset, 8, "Index node truncated");
    // 左兄弟页号。
    node.leftGen = take(record, offset, 8, "Index node truncated");
    // 左兄弟代数。
    node.rightId = take(record, offset, 8, "Index node truncated");
    // 右兄弟页号。
    node.rightGen = take(record, offset, 8, "Index node truncated");
    // 右兄弟代数。
    node.keys.reserve(keyCount);
    // 预留键空间。
    for (std::size_t i = 0; i < keyCount; ++i) {
    // 逐个键解析。
        if (offset >= record.size()) fail("Index node key truncated");
        // 连列数都没有说明截断。
        const auto components = record[offset++];
        // 该键的列数。
        IndexKey key;
        // 复合键。
        key.values.reserve(components);
        // 预留列空间。
        for (std::size_t j = 0; j < components; ++j) key.values.push_back(decodeValue(record, offset));
        // 逐列解析。
        node.keys.push_back(std::move(key));
        // 收进键列表。
    }
    if (node.leaf) {
    // 叶子接下来是行引用。
        node.values.reserve(keyCount);
        // 预留。
        for (std::size_t i = 0; i < keyCount; ++i) {
        // 每个键一个行引用。
            RowRef ref{{take(record, offset, 8, "Index RowRef truncated"), take(record, offset, 8, "Index RowRef truncated")},
                       {static_cast<std::uint32_t>(take(record, offset, 4, "Index RowRef truncated")), take(record, offset, 8, "Index RowRef truncated")}};
            // 依次读页号、页代数、槽号、槽代数。
            node.values.push_back(ref);
            // 收进值列表。
        }
    } else {
    // 内部节点接下来是子页引用。
        node.children.reserve(keyCount + 1);
        // 子节点数恒为键数加一。
        for (std::size_t i = 0; i < keyCount + 1; ++i) {
        // 逐个读。
            const PageId id = take(record, offset, 8, "Index child truncated");
            // 子页号。
            const auto generation = take(record, offset, 8, "Index child truncated");
            // 子页代数。
            node.children.push_back({id, generation});
            // 收进子节点列表。
        }
    }
    return node;
    // 返回解析结果。
}

std::size_t PageBPlusTree::childIndex(const std::vector<IndexKey>& keys, const IndexKey& key) const {
// 内部节点里应该走哪个子节点：第一个大于该键的位置。
    return static_cast<std::size_t>(std::upper_bound(keys.begin(), keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return compareKey(left, right) < 0; }) - keys.begin());
}
std::size_t PageBPlusTree::lowerBound(const std::vector<IndexKey>& keys, const IndexKey& key) const {
// 叶子内第一个不小于该键的位置。
    return static_cast<std::size_t>(std::lower_bound(keys.begin(), keys.end(), key,
        [](const IndexKey& left, const IndexKey& right) { return compareKey(left, right) < 0; }) - keys.begin());
}

PageRef PageBPlusTree::locateLeaf(PageRef root) const {
// 一直走最左子节点，到达第一个叶子。
    auto current = root;
    for (;;) {
    // 循环直到叶子。
        auto node = readNode(current);
        // 读当前节点。
        if (node.leaf) return current;
        // 是叶子就返回。
        current = node.children.front();
        // 否则走第一个子节点。
    }
}
PageRef PageBPlusTree::liftLeaf(PageRef root, const IndexKey& key) const {
// 按某个键下钻到对应叶子。
    auto current = root;
    for (;;) {
    // 循环直到叶子。
        auto node = readNode(current);
        // 读当前节点。
        if (node.leaf) return current;
        // 是叶子就返回。
        current = node.children[childIndex(node.keys, key)];
        // 按比较结果选择子节点。
    }
}

bool PageBPlusTree::insert(IndexKey key, RowRef row) {
// 插入入口：处理空树与唯一性，然后交给递归插入。
    if (!exists()) create();
    // 索引不存在则先建立。
    if (unique_ && !search(key).empty()) return false;
    // 唯一索引遇到重复键直接返回失败。
    PageRef root;
    std::size_t size;
    // 当前索引项数。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) {
    // 空树：第一次插入直接建一个叶子当根。
        auto ref = buffer_.allocate(owner_);
        // 分配页。
        Node leaf;
        // 新节点。
        leaf.leaf = true;
        // 它是叶子。
        leaf.height = 0;
        // 叶子高度为 0。
        leaf.keys = {key};
        // 放入这一个键。
        leaf.values = {row};
        // 放入对应行。
        replaceNode(ref, leaf);
        // 写入页。
        writeMeta(ref, 1);
        // 元页记录根与大小 1。
        return true;
        // 插入成功。
    }
    std::optional<std::pair<IndexKey, PageRef>> split;
    // 根分裂时返回的提升键与右兄弟。
    std::optional<PageRef> rootReplace;
    // 需要换根时的新根引用。
    insertInto(root, key, row, true, split, rootReplace);
    // 递归插入。
    auto newRoot = rootReplace ? *rootReplace : root;
    // 换了根就用新根。
    writeMeta(newRoot, size + 1);
    // 更新元页。
    return true;
    // 插入成功。
}

void PageBPlusTree::insertInto(PageRef node, const IndexKey& key, RowRef row, bool isRoot,
                               std::optional<std::pair<IndexKey, PageRef>>& split,
                               std::optional<PageRef>& rootReplace) {
// 递归插入：叶子直接插，节点满就分裂，并把提升键与右兄弟交给父节点。
    auto current = readNode(node);
    // 先读出当前节点。
    if (current.leaf) {
    // 叶子分支。
        const auto position = lowerBound(current.keys, key);
        // 找到插入位置，保证相同键保持稳定顺序。
        current.keys.insert(current.keys.begin() + static_cast<std::ptrdiff_t>(position), key);
        // 插入键。
        current.values.insert(current.values.begin() + static_cast<std::ptrdiff_t>(position), row);
        // 同位置插入行，保持一一对应。
        split.reset();
        // 默认认为不需要向上传播分裂。
        rootReplace.reset();
        // 同样不考虑换根。
        if (current.keys.size() <= maxKeys_) { replaceNode(node, current); return; }
        // 未超上限，写回即可。
        // Leaf split：左保留 [0, mid)，右为 [mid..)
        const auto middle = current.keys.size() / 2;
        // 分裂点。
        Node right;
        // 新右兄弟。
        right.leaf = true;
        // 它也是叶子。
        right.height = current.height;
        // 高度与左一致。
        right.parentId = current.parentId; right.parentGen = current.parentGen;
        // 先沿用左节点的父指针，父节点稍后会修正。
        right.leftId = node.id; right.leftGen = node.generation;
        // 右兄弟的左指针指向原节点。
        right.rightId = current.rightId; right.rightGen = current.rightGen;
        // 右兄弟接管原来的右兄弟。
        right.keys.assign(current.keys.begin() + static_cast<std::ptrdiff_t>(middle), current.keys.end());
        // 后半键搬到右兄弟。
        right.values.assign(current.values.begin() + static_cast<std::ptrdiff_t>(middle), current.values.end());
        // 对应行也搬走。
        current.keys.resize(middle);
        // 左节点只留前半。
        current.values.resize(middle);
        // 行同步截断。
        auto rightRef = buffer_.allocate(owner_);
        // 为右兄弟分配页。
        // 让原右兄弟的左指针指向新的右节点。
        if (current.rightId != 0) {
        // 原来有右兄弟才需要改它的左指针。
            mutateNode({current.rightId, current.rightGen}, [&](Node& sibling) {
                sibling.leftId = rightRef.id; sibling.leftGen = rightRef.generation;
            });
            // 读改写三步完成指针修正。
        }
        current.rightId = rightRef.id; current.rightGen = rightRef.generation;
        // 左节点的右指针指向新的右兄弟。
        replaceNode(node, current);
        // 写回左节点。
        replaceNode(rightRef, right);
        // 写入右兄弟。
        if (!isRoot) {
        // 非根节点：把提升键与右兄弟交给父节点。
            split = {{right.keys.front(), rightRef}};
            // 叶子分裂时把右兄弟第一个键复制上去做分隔键。
        } else {
        // 根叶子分裂 → 新建内部节点根，两个叶子的父指针都指向新根。
            const auto oldRoot = readNode(node);
            // 重新读一遍左节点。
            Node root;
            // 新根。
            root.leaf = false;
            // 内部节点。
            root.height = oldRoot.height + 1;
            // 高度加一。
            root.keys = {right.keys.front()};
            // 分隔键。
            root.children = {node, rightRef};
            // 两个叶子成为子节点。
            const auto rootRef = buffer_.allocate(owner_);
            // 分配根页。
            mutateNode(node, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
            // 修正左孩子的父指针。
            mutateNode(rightRef, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
            // 修正右孩子的父指针。
            replaceNode(rootRef, root);
            // 写入新根。
            rootReplace = rootRef;
            // 通过出参告诉上层换根。
        }
        return;
        // 叶子处理结束。
    }
    const auto position = childIndex(current.keys, key);
    // 决定下钻的子节点。
    const auto child = current.children[position];
    // 取子页引用。
    std::optional<std::pair<IndexKey, PageRef>> childSplit;
    // 子层可能返回的分裂结果。
    std::optional<PageRef> childRootReplace;
    // 子层可能返回的新根（子层不是根时不会用到）。
    insertInto(child, key, row, false, childSplit, childRootReplace);
    // 递归插入。
    split.reset();
    // 先清空本层结果。
    rootReplace.reset();
    // 同上。
    if (!childSplit) return;  // 子树全量接收，无需传播
    // 子节点没分裂，本层无需改动。
    const auto& promoted = childSplit->first;
    // 子层提升上来的分隔键。
    const auto& rightRef = childSplit->second;
    // 子层的右兄弟页。
    current.keys.insert(current.keys.begin() + static_cast<std::ptrdiff_t>(position), promoted);
    // 把提升键插到分隔位置。
    current.children.insert(current.children.begin() + static_cast<std::ptrdiff_t>(position + 1), rightRef);
    // 右兄弟插在对应子节点之后。
    mutateNode(rightRef, [&](Node& rightNode) { rightNode.parentId = node.id; rightNode.parentGen = node.generation; });
    // 修正右兄弟的父指针。
    replaceNode(node, current);
    // 写回当前节点。
    if (current.keys.size() <= maxKeys_) return;
    // 未超上限则结束。
    const auto middle = current.keys.size() / 2;
    // 内部节点分裂点。
    const IndexKey promotedKey = current.keys[middle];
    // 中间键将被提升到父层。
    Node right;
    // 新右兄弟。
    right.leaf = false;
    // 内部节点。
    right.height = current.height;
    // 高度相同。
    right.parentId = current.parentId; right.parentGen = current.parentGen;
    // 先沿用父指针。
    right.keys.assign(current.keys.begin() + static_cast<std::ptrdiff_t>(middle + 1), current.keys.end());
    // 中间键右边的键搬走。
    right.children.assign(current.children.begin() + static_cast<std::ptrdiff_t>(middle + 1), current.children.end());
    // 对应的子指针也搬走。
    current.keys.resize(middle);
    // 左节点保留中间键之前的键。
    current.children.resize(middle + 1);
    // 左节点子指针数保持键数加一。
    auto rightNodeRef = buffer_.allocate(owner_);
    // 分配右兄弟页。
    for (const auto& moved : right.children) {
    // 被搬走的子节点需要改父指针。
        mutateNode(moved, [&](Node& movedNode) { movedNode.parentId = rightNodeRef.id; movedNode.parentGen = rightNodeRef.generation; });
        // 指向新的父页。
    }
    replaceNode(node, current);
    // 写回左节点。
    replaceNode(rightNodeRef, right);
    // 写入右兄弟。
    if (!isRoot) {
    // 非根：把提升键与右兄弟交给父层。
        split = {{promotedKey, rightNodeRef}};
        // 内部节点分裂是把中间键提升（而不是复制）。
    } else {
    // 根内节点分裂 → 新建内部节点根，promotedKey 上提
        const auto oldRootHeight = current.height;
        // 原根高度。
        Node root;
        // 新根。
        root.leaf = false;
        // 内部节点。
        root.height = oldRootHeight + 1;
        // 高度加一。
        root.keys = {promotedKey};
        // 唯一分隔键。
        root.children = {node, rightNodeRef};
        // 左右两半成为子节点。
        const auto rootRef = buffer_.allocate(owner_);
        // 分配根页。
        mutateNode(node, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
        // 修正左孩子父指针。
        mutateNode(rightNodeRef, [&](Node& child) { child.parentId = rootRef.id; child.parentGen = rootRef.generation; });
        // 修正右孩子父指针。
        replaceNode(rootRef, root);
        // 写入新根。
        rootReplace = rootRef;
        // 通知上层换根。
    }
}

bool PageBPlusTree::erase(IndexKey key, RowRef row) {
// 删除入口：处理空树，并在删除后收缩根。
    if (!exists()) return false;
    // 索引不存在就没得删。
    PageRef root;
    std::size_t size;
    // 当前项数。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return false;
    // 空树没有可删内容。
    std::optional<PageRef> rootReplace;
    // 需要换根时的新根。
    bool rootUnderflow = false;
    // 根下溢标记（根不受最小键数约束）。
    const bool found = eraseInto(root, key, row, root, rootReplace, rootUnderflow);
    // 递归删除。
    if (!found) return false;
    // 没删到就原样返回。
    // 若根内节点只剩单个子节点，则收缩根（高度减一）
    if (!rootReplace) {
    // 递归过程中没有换根时才需要考虑收缩。
        const auto rootNode = readNode(root);
        // 读根。
        if (!rootNode.leaf && rootNode.keys.empty() && rootNode.children.size() == 1) {
        // 根是空的内部节点且只剩一个孩子。
            const auto child = rootNode.children.front();
            // 取唯一的孩子。
            mutateNode(child, [](Node& c) { c.parentId = 0; c.parentGen = 0; });
            // 让它变成新的根，父指针清空。
            buffer_.release(root);
            // 回收旧根页。
            rootReplace = child;
            // 记录新根。
        }
    }
    writeMeta(rootReplace ? *rootReplace : root, size - 1);
    // 更新元页中的根与大小。
    return true;
    // 删除成功。
}

bool PageBPlusTree::eraseInto(PageRef node, const IndexKey& key, RowRef row, const PageRef& root,
                              std::optional<PageRef>& rootReplace, bool& underflow) {
// 递归删除：找到并删除叶子中的项，再向上处理下溢。
    auto current = readNode(node);
    // 读当前节点。
    if (current.leaf) {
    // 叶子分支。
        const auto position = lowerBound(current.keys, key);
        // 定位到第一个不小于目标键的位置。
        bool erased = false;
        // 是否真的删掉了一项。
        for (std::size_t i = position; i < current.keys.size(); ++i) {
        // 从该位置往后找。
            if (compareKey(current.keys[i], key) != 0) break;
            // 键不再相等就可以停止，说明没有匹配项。
            if (sameRow(current.values[i], row)) {
            // 键与行都匹配才算命中；非唯一索引需要靠行来区分。
                current.keys.erase(current.keys.begin() + static_cast<std::ptrdiff_t>(i));
                // 删除键。
                current.values.erase(current.values.begin() + static_cast<std::ptrdiff_t>(i));
                // 删除对应行。
                erased = true;
                // 标记已删除。
                break;
                // 一项只删一次。
            }
        }
        replaceNode(node, current);
        // 无论是否删除都写回，保持页内容与内存一致。
        underflow = false;
        // 默认不下溢。
        if (!erased) return false;
        // 没删到，向上报告失败。
        if (!samePage(node, root)) underflow = current.keys.size() < minKeys_;
        // 非根节点键数低于下限就是下溢，需要借位或合并。
        return true;
        // 删除成功。
    }
    const auto position = childIndex(current.keys, key);
    // 决定下钻的子节点。
    const auto child = current.children[position];
    // 子页引用。
    bool childUnderflow = false;
    // 子节点的下溢标记。
    const bool found = eraseInto(child, key, row, root, rootReplace, childUnderflow);
    // 递归删除。
    if (!found) { underflow = false; return false; }
    // 下层没删到，本层也不变。
    if (childUnderflow) rebalanceChild(current, position, child, samePage(node, root));
    // 子节点下溢时做借位或合并，可能修改 current 的键与子指针。
    replaceNode(node, current);
    // 写回当前节点。
    underflow = false;
    // 默认不下溢。
    if (!samePage(node, root) && current.keys.size() < minKeys_) underflow = true;
    // 非根节点键数不足则上报下溢。
    return true;
    // 删除成功。
}

bool PageBPlusTree::rebalanceChild(Node& n, std::size_t childIdx, PageRef childRef, bool nodeIsRoot) {
// 子节点下溢后的修复：先向右兄弟借，再向左兄弟借，都不行就合并。
    auto child = readNode(childRef);
    // 读出下溢的子节点。
    const std::size_t min = minKeys_;
    // 键数下限。
    // 1) 向右兄弟借：右兄弟首条目移入本节点末尾
    if (childIdx + 1 < n.children.size()) {
    // 存在右兄弟。
        const auto rightRef = n.children[childIdx + 1];
        // 右兄弟引用。
        auto right = readNode(rightRef);
        // 读出右兄弟。
        if (right.keys.size() > min) {
        // 右兄弟键数富余，可以借一个。
            if (child.leaf) {
            // 叶子借位：直接把右兄弟第一个键和行搬过来。
                child.keys.push_back(right.keys.front());
                // 键移到末尾，保持叶子内有序。
                child.values.push_back(right.values.front());
                // 行同步搬移。
                right.keys.erase(right.keys.begin());
                // 右兄弟删掉第一个键。
                right.values.erase(right.values.begin());
                // 行同步删除。
                n.keys[childIdx] = right.keys.front();
                // 父节点中分隔左右的分隔键要更新为右兄弟新的第一个键。
            } else {
            // 内部节点借位：需要经过父节点的分隔键。
                child.children.push_back(right.children.front());
                // 右兄弟的第一个子节点过继给左。
                child.keys.push_back(n.keys[childIdx]);
                // 父节点的分隔键下沉到左节点末尾。
                mutateNode(right.children.front(), [&](Node& moved) {
                    moved.parentId = childRef.id; moved.parentGen = childRef.generation;
                });
                // 过继过来的子节点要改父指针。
                n.keys[childIdx] = right.keys.front();
                // 父节点的分隔键上提为右兄弟的第一个键。
                right.keys.erase(right.keys.begin());
                // 右兄弟删掉该键。
                right.children.erase(right.children.begin());
                // 对应的子指针也删掉。
            }
            replaceNode(childRef, child);
            // 写回借位后的左节点。
            replaceNode(rightRef, right);
            // 写回借出后的右兄弟。
            return !nodeIsRoot && n.keys.size() < min;
            // 借位不会改变父节点键数，但仍需上报父节点自身是否下溢。
        }
    }
    // 2) 向左兄弟借：左兄弟末条目移入本节点开头
    if (childIdx > 0) {
    // 存在左兄弟。
        const auto leftRef = n.children[childIdx - 1];
        // 左兄弟引用。
        auto left = readNode(leftRef);
        // 读出左兄弟。
        if (left.keys.size() > min) {
        // 左兄弟富余。
            if (child.leaf) {
            // 叶子借位。
                child.keys.insert(child.keys.begin(), left.keys.back());
                // 左兄弟最后一个键插到本节点开头。
                child.values.insert(child.values.begin(), left.values.back());
                // 行同步。
                left.keys.pop_back();
                // 左兄弟删除末尾键。
                left.values.pop_back();
                // 行同步删除。
                n.keys[childIdx - 1] = child.keys.front();
                // 更新父节点的分隔键为本节点新的第一个键。
            } else {
            // 内部节点借位。
                child.keys.insert(child.keys.begin(), n.keys[childIdx - 1]);
                // 父节点分隔键下沉到本节点开头。
                child.children.insert(child.children.begin(), left.children.back());
                // 左兄弟最后一个子节点过继过来。
                mutateNode(left.children.back(), [&](Node& moved) {
                    moved.parentId = childRef.id; moved.parentGen = childRef.generation;
                });
                // 修正父指针。
                n.keys[childIdx - 1] = left.keys.back();
                // 左兄弟的最后一个键上提到父节点。
                left.keys.pop_back();
                // 左兄弟删掉该键。
                left.children.pop_back();
                // 对应的子指针也删掉。
            }
            replaceNode(childRef, child);
            // 写回本节点。
            replaceNode(leftRef, left);
            // 写回左兄弟。
            return !nodeIsRoot && n.keys.size() < min;
            // 上报父节点状态。
        }
    }
    // 3) 无法借位 → 合并。优先并入左兄弟，否则并入右兄弟。
    const bool intoLeft = childIdx > 0;
    // 有左兄弟就并入左边。
    const auto keepRef = intoLeft ? n.children[childIdx - 1] : childRef;
    // 保留的页。
    const auto dropRef = intoLeft ? childRef : n.children[childIdx + 1];
    // 被合并后要回收的页。
    auto keep = readNode(keepRef);
    // 读出保留节点。
    auto drop = readNode(dropRef);
    // 读出待合并节点。
    const auto pivot = intoLeft ? n.keys[childIdx - 1] : n.keys[childIdx];
    // 合并时要拉下来的父节点分隔键。
    if (keep.leaf) {
    // 叶子合并：键与行直接拼接。
        keep.keys.insert(keep.keys.end(), drop.keys.begin(), drop.keys.end());
        // 拼键。
        keep.values.insert(keep.values.end(), drop.values.begin(), drop.values.end());
        // 拼行。
        keep.rightId = drop.rightId; keep.rightGen = drop.rightGen;
        // 接管被合并节点的右兄弟。
    } else {
    // 内部节点合并：中间要插入父节点分隔键。
        keep.keys.push_back(pivot);
        // 分隔键放在中间。
        keep.keys.insert(keep.keys.end(), drop.keys.begin(), drop.keys.end());
        // 再接上被合并节点的键。
        keep.children.insert(keep.children.end(), drop.children.begin(), drop.children.end());
        // 子指针同步拼接。
        for (const auto& moved : drop.children) {
        // 过继来的子节点要改父指针。
            mutateNode(moved, [&](Node& m) { m.parentId = keepRef.id; m.parentGen = keepRef.generation; });
            // 指向新的父页。
        }
        keep.rightId = drop.rightId; keep.rightGen = drop.rightGen;
        // 接管右兄弟。
    }
    // 重连叶链：被删页右侧的兄弟左指改为 keep
    if (drop.rightId != 0) {
    // 被合并节点还有右兄弟。
        mutateNode({drop.rightId, drop.rightGen}, [&](Node& r) { r.leftId = keepRef.id; r.leftGen = keepRef.generation; });
        // 把它的左指针改指到保留节点，叶链不会断开。
    }
    replaceNode(keepRef, keep);
    // 写回合并结果。
    buffer_.release(dropRef);
    // 回收被合并的页。
    if (intoLeft) {
    // 并入左兄弟时，父节点要删除对应的分隔键与子指针。
        n.keys.erase(n.keys.begin() + static_cast<std::ptrdiff_t>(childIdx - 1));
        // 删除分隔键。
        n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(childIdx));
        // 删除对应子指针。
    } else {
    // 并入右兄弟时同理，但下标不同。
        n.keys.erase(n.keys.begin() + static_cast<std::ptrdiff_t>(childIdx));
        // 删除分隔键。
        n.children.erase(n.children.begin() + static_cast<std::ptrdiff_t>(childIdx + 1));
        // 删除对应子指针。
    }
    return !nodeIsRoot && n.keys.size() < min;
    // 合并会让父节点少一个键，可能引发父节点下溢。
}

std::vector<RowRef> PageBPlusTree::search(const IndexKey& key) const {
// 等值查找：下钻到叶子后收集所有相等项。
    if (!exists()) return {};
    // 索引不存在直接返回空。
    PageRef root;
    std::size_t size;
    // 项数。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return {};
    // 空树返回空。
    std::vector<RowRef> rows;
    // 结果。
    auto current = root;
    for (;;) {
    // 循环下钻。
        auto node = readNode(current);
        // 读节点。
        if (node.leaf) {
        // 到叶子。
            const auto position = lowerBound(node.keys, key);
            // 第一个不小于目标键的位置。
            for (std::size_t i = position; i < node.keys.size(); ++i) {
            // 从该位置往后收集。
                if (compareKey(node.keys[i], key) != 0) break;
                // 不再相等就结束。
                rows.push_back(node.values[i]);
                // 收集行引用。
            }
            return rows;
        }
        current = node.children[childIndex(node.keys, key)];
        // 继续下钻。
    }
}

std::vector<RowRef> PageBPlusTree::range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                                         const std::optional<IndexKey>& upper, bool upperInclusive) const {
// 范围查找：从下界所在叶子开始，沿叶链向右扫描到超过上界。
    std::vector<RowRef> rows;
    // 结果。
    if (!const_cast<PageBPlusTree*>(this)->exists()) return rows;
    // const 方法里需要惰性查找元页，因此用 const_cast 调用非 const 版本。
    PageRef root;
    std::size_t size;
    // 项数。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return rows;
    // 空树返回空。
    auto current = lower ? liftLeaf(root, *lower) : locateLeaf(root);
    // 有下界就从下界所在叶子开始，否则从最左叶子开始。
    std::size_t guard = 0;
    // 防止叶链成环导致死循环的计数。
    while (current.id != 0) {
    // 沿 leaf 的右指针前进，直到没有右兄弟。
        auto node = const_cast<PageBPlusTree*>(this)->readNode(current);
        // 读当前叶子。
        bool stop = false;
        // 是否已经越过上界。
        for (std::size_t i = 0; i < node.keys.size(); ++i) {
        // 逐个键判断。
            if (lower) {
            // 有下界时过滤。
                const auto order = compareKey(node.keys[i], *lower);
                // 与下界比较。
                if (order < 0 || (order == 0 && !lowerInclusive)) continue;
                // 小于下界，或者等于下界但要求开区间，跳过。
            }
            if (upper) {
            // 有上界时判断是否该停。
                const auto order = compareKey(node.keys[i], *upper);
                // 与上界比较。
                if (order > 0 || (order == 0 && !upperInclusive)) { stop = true; break; }
                // 超过上界，或者等于上界但要求开区间，停止扫描。
            }
            rows.push_back(node.values[i]);
            // 通过筛选，收集这一行。
        }
        if (stop) break;
        // 已越过上界。
        if (++guard > file_->pagesFor(owner_).size() + 1u) fail("Index leaf chain loop");
        // 走的页数超过总页数说明叶链成环。
        current = {node.rightId, node.rightGen};
        // 走到下一个叶子。
    }
    return rows;
}

std::size_t PageBPlusTree::size() const {
// 取索引项总数，读自元页。
    PageRef root;
    // 根引用（不需要）。
    std::size_t size = 0;
    // 结果。
    if (const_cast<PageBPlusTree*>(this)->exists()) requireMeta(root, size);
    // 索引存在时读元页得到大小。
    return size;
    // 返回。
}
std::size_t PageBPlusTree::height() const {
// 取树高：根节点记录的 height 加一。
    PageRef root;
    std::size_t size;
    // 项数（不需要）。
    if (!const_cast<PageBPlusTree*>(this)->exists()) return 0;
    // 索引不存在高度为 0。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return 0;
    // 空树高度为 0。
    const auto rootNode = const_cast<PageBPlusTree*>(this)->readNode(root);
    // 读根节点。
    return static_cast<std::size_t>(rootNode.height) + 1;
    // 叶子高度字段是 0，因此对外高度是字段加一。
}
std::size_t PageBPlusTree::pageCount() const { return file_->pagesFor(owner_).size(); }
// 该索引占用的页数，直接问页文件。
bool PageBPlusTree::validate() const {
// 结构自检入口。
    if (!exists()) return true;
    // 不存在的索引视为通过。
    PageRef root;
    std::size_t size = 0;
    // 元页里记录的项数。
    requireMeta(root, size);
    if (root.id == kInvalidPageId) return size == 0;
    // 空树时大小必须为 0。
    std::size_t leafDepth = 0;
    // 记录叶子深度。
    if (!validateNode(root, 1, std::nullopt, std::nullopt, leafDepth)) return false;
    // 递归校验键序、子节点数与平衡性。
    // 遍历叶链统计行数，校验元数据 size 与叶链完整性
    std::size_t counted = 0;
    // 实际统计到的键数。
    auto leaf = locateLeaf(root);
    // 从最左叶子开始。
    std::size_t guard = file_->pagesFor(owner_).size() + 2u;
    // 防环计数。
    while (leaf.id != 0) {
    // 沿叶链走。
        const auto node = readNode(leaf);
        // 读叶子。
        if (!node.leaf) return false;
        // 叶链里混入内部节点属于损坏。
        counted += node.keys.size();
        // 累加键数。
        leaf = {node.rightId, node.rightGen};
        // 下一个叶子。
        if (--guard == 0) return false;
        // 走太多步说明成环。
    }
    return counted == size;
    // 叶链统计值必须与元页记录一致。
}
IndexInspect PageBPlusTree::inspect() const {
// 巡检：给出逐页明细与若干一致性结论，供工作台展示。
    IndexInspect result;
    // 结果对象。
    const auto* self = this;
    // 便于下面的 const_cast 写法。
    if (!const_cast<PageBPlusTree*>(self)->exists()) return result;
    // 索引不存在则 present 保持 false。
    PageRef root;
    std::size_t size;
    // 项数。
    requireMeta(root, size);
    result.present = true;
    // 标记存在。
    if (meta_) result.meta = *meta_;
    // 记录元页位置。
    result.root = root;
    // 记录根位置。
    if (root.id == kInvalidPageId) { result.rootReachable = true; return result; }
    // 空树视为自洽。

    std::vector<PageRef> leaves;
    // 收集叶子页。
    bool reachable = true;
    // 遍历是否终止。
    bool parentsValid = true;
    // 父指针是否自洽。
    std::size_t visited = 0;
    // 已访问节点计数。
    std::function<void(PageRef)> visit = [&](PageRef ref) {
    // 深度优先遍历。
        if (++visited > maxKeys_ * 4 + 512) { reachable = false; return; }
        // 访问次数异常增大说明结构可能成环，停止遍历。
        const auto node = readNode(ref);
        // 读节点。
        IndexPageInfo info;
        // 该页的快照。
        info.page = ref;
        // 页引用。
        info.leaf = node.leaf;
        // 叶子标志。
        info.height = node.height;
        // 高度字段。
        info.keyCount = node.keys.size();
        // 键数量。
        info.parent = {node.parentId, node.parentGen};
        // 父指针。
        info.left = {node.leftId, node.leftGen};
        // 左兄弟。
        info.right = {node.rightId, node.rightGen};
        // 右兄弟。
        result.pages.push_back(info);
        // 收进明细。
        if (node.leaf) { leaves.push_back(ref); return; }
        // 叶子不再下钻。
        for (const auto& child : node.children) {
        // 遍历子节点。
            const auto childNode = readNode(child);
            // 读子节点。
            if (childNode.parentId != ref.id || childNode.parentGen != ref.generation) parentsValid = false;
            // 子节点的父指针必须指回当前页，否则父链断裂。
            visit(child);
            // 递归。
        }
    };
    visit(root);
    result.nodeCount = result.pages.size();
    // 节点页数。
    result.leafCount = leaves.size();
    // 叶子页数。
    result.parentLinksValid = parentsValid;
    // 父链结论。
    result.rootReachable = reachable;
    // 可达性结论。
    result.height = result.pages.empty() ? 0 : static_cast<std::size_t>(readNode(root).height) + 1;
    // 树高。

    // 叶链：从最左叶沿 right 指针顺数，校验右指成链、左右指互成对、且首尾闭合
    bool chainOk = true;
    // 叶链是否完好。
    if (result.height > 0) {
    // 非空树才检查。
        const auto firstLeef = locateLeaf(root);
        // 最左叶子。
        const auto firstNode = readNode(firstLeef);
        // 读它。
        if (firstNode.leftId != 0 || firstNode.leftGen != 0) { chainOk = false; reachable = false; }
        // 最左叶子的左指针必须为空。
        result.rowCount += firstNode.keys.size();
        // 先累加第一个叶子的键数。
        std::size_t chain = 1;
        // 叶链长度。
        result.leafChainLength = 1;
        // 记录。
        auto current = firstLeef;
        // 从最左叶子开始。
        while (true) {
        // 沿右指针前进。
            const auto node = readNode(current);
            // 读当前叶子。
            if (node.rightId == 0) break;
            // 没有右兄弟说明到链尾。
            const auto nextRef = PageRef{node.rightId, node.rightGen};
            // 下一个叶子。
            const auto nextNode = readNode(nextRef);
            // 读它。
            if (nextNode.leftId != current.id || nextNode.leftGen != current.generation) chainOk = false;
            // 下一个叶子的左指针必须指回当前，否则链是单向的。
            current = nextRef;
            // 前进。
            result.rowCount += nextNode.keys.size();
            // 累加键数。
            ++chain;
            // 链长加一。
            result.leafChainLength = chain;
            // 记录。
            if (chain > result.nodeCount + 2) { reachable = false; chainOk = false; break; }
            // 链长超过节点总数说明成环。
        }
    }
    result.leafChainLinked = chainOk && reachable;
    // 叶链结论。
    result.rootReachable = reachable;
    // 可达性结论。
    if (!result.leafChainLinked) result.problems.push_back("leaf chain left/right pointers broken");
    // 记录问题。
    if (!parentsValid) result.problems.push_back("child parent pointers mismatch");
    // 记录问题。
    if (!reachable) result.problems.push_back("tree traversal did not terminate");
    // 记录问题。
    return result;
    // 返回巡检结果。
}
bool PageBPlusTree::validateNode(PageRef ref, std::uint32_t depth, std::optional<const IndexKey*> left,
                                 std::optional<const IndexKey*> right, std::size_t& leafDepth) const {
// 递归校验：键有序、键落在上下界内、子节点数正确、叶子深度一致。
    const auto node = readNode(ref);
    // 读节点。
    if (node.keys.size() > maxKeys_) return false;
    // 键数超上限直接失败。
    for (std::size_t i = 1; i < node.keys.size(); ++i) {
    // 检查节点内键是否严格递增。
        if (compareKey(node.keys[i - 1], node.keys[i]) >= 0) return false;
        // 出现相等或逆序即为不合法。
    }
    for (std::size_t i = 0; i < node.keys.size(); ++i) {
    // 检查每个键是否落在父节点给定的区间内。
        if (left && compareKey(node.keys[i], **left) < 0) return false;
        // 不得小于下界键。
        if (right && compareKey(node.keys[i], **right) > 0) return false;
        // 不得大于上界键。
    }
    if (node.leaf) {
    // 叶子分支。
        if (leafDepth == 0) leafDepth = depth;
        // 第一次到叶子记录深度。
        return leafDepth == depth;
        // 所有叶子必须同深度。
    }
    if (node.children.size() != node.keys.size() + 1) return false;
    // 内部节点子指针数必须是键数加一。
    for (std::size_t i = 0; i < node.children.size(); ++i) {
    // 递归校验每个子节点。
        std::optional<const IndexKey*> lo = i > 0 ? std::optional<const IndexKey*>(&node.keys[i - 1]) : left;
        // 子节点的下界取左边的分隔键，第一个子节点沿用本节点下界。
        std::optional<const IndexKey*> hi = i < node.keys.size() ? std::optional<const IndexKey*>(&node.keys[i]) : right;
        // 上界取右边的分隔键，最后一个子节点沿用本节点上界。
        if (!validateNode(node.children[i], depth + 1, lo, hi, leafDepth)) return false;
        // 递归。
    }
    return true;
    // 全部通过。
}
}  // namespace minisql::storage
