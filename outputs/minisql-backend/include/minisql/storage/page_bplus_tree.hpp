#pragma once
#include "minisql/storage/bplus_tree.hpp"
#include "minisql/storage/buffer_pool.hpp"
#include "minisql/storage/heap.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace minisql::storage {
// 页级 B+ 树（X20）。每个索引 = owner 下一个 INDEX_META 元页 + 若干节点页。
// 节点页内只有一个二进制节点记录，父/子/兄弟均以 PageRef 指向页。
enum IndexPageType : std::uint8_t { IndexMeta = 0x50, IndexInternal = 0x41, IndexLeaf = 0x42 };
// 页类型标记：0x50 元页、0x41 内部节点页、0x42 叶子页。

// 页级结构校验输出（每页一条；页类型/height/keyCount/父/兄弟指针，用于工作台诊断命令）。
struct IndexPageInfo {
// 单个索引页的快照信息。
    PageRef page;
    // 该页的引用。
    bool leaf = true;
    // 是否叶子页。
    std::uint32_t height = 0;
    // 页内记录的树高字段。
    std::size_t keyCount = 0;
    // 该页的键数量。
    PageRef parent{0, 0};
    // 父页引用，0 表示没有父页。
    PageRef left{0, 0};
    // 左兄弟页引用。
    PageRef right{0, 0};
    // 右兄弟页引用。
};
struct IndexInspect {
// 整棵索引的巡检结果。
    bool present = false;
    // 索引是否存在。
    PageRef meta{0, 0};
    // 元页引用。
    PageRef root{0, 0};
    // 根页引用。
    std::size_t height = 0;          // 树高（叶深计数）
    // 树高。
    std::size_t nodeCount = 0;       // 节点页数
    // 节点页总数。
    std::size_t leafCount = 0;
    // 叶子页数量。
    std::size_t rowCount = 0;        // 叶链内总键数
    // 叶链上的键总数，应与元页记录的 size 一致。
    std::size_t leafChainLength = 0;
    // 叶链长度。
    bool rootReachable = false;      // 从根可完整遍历
    // 能否从根遍历完整棵树。
    bool leafChainLinked = false;    // 叶链左右指针成链
    // 叶链的左右指针是否互相吻合。
    bool parentLinksValid = false;   // 所有子页父指针都与父页一致
    // 父子指针是否自洽。
    std::vector<std::string> problems;
    // 发现的问题列表。
    std::vector<IndexPageInfo> pages;
    // 逐页明细。
};

class PageBPlusTree {
// 页级 B+ 树：节点真正落在 4KB 页里，通过缓冲池读写，可跨进程持久。
public:
    PageBPlusTree(std::shared_ptr<PageFile> file, BufferPool& buffer, std::uint64_t owner,
                  std::size_t maxKeys, bool unique);
    // 构造：owner 标识这批页属于哪个索引，maxKeys 是节点键数上限。
    PageBPlusTree(const PageBPlusTree&) = delete;
    // 持有缓冲池引用与页状态，禁止拷贝。
    PageBPlusTree& operator=(const PageBPlusTree&) = delete;
    // 同样禁止拷贝赋值。

    bool exists() const;
    // 索引是否已建立（能否找到元页）。
    bool create();                       // 无元页时创建，返回是否新建
    // 创建元页；已存在则返回 false。
    bool insert(IndexKey key, RowRef row);
    // 插入一个索引项；唯一索引重复时返回 false。
    bool erase(IndexKey key, RowRef row);   // 删除并触发页内借位/合并与根收缩
    // 删除指定索引项，必要时做借位、合并与根收缩。
    std::vector<RowRef> search(const IndexKey& key) const;
    // 等值查找。
    std::vector<RowRef> range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                              const std::optional<IndexKey>& upper, bool upperInclusive) const;
    // 范围查找，沿叶链顺序扫描。
    std::size_t size() const;
    // 索引项总数，读自元页。
    std::size_t height() const;
    // 树高。
    std::size_t pageCount() const;
    // 该索引占用的页数。
    bool validate() const;
    // 结构自检：键序、平衡、叶链完整、计数一致。
    IndexInspect inspect() const;   // 页级结构校验：页类型/height/keyCount/父/兄弟/叶链/根可达
    // 巡检入口，供工作台与诊断命令使用。

private:
    struct Node {
    // 节点在内存中的表示，序列化后才写入页。
        bool leaf = true;
        // 是否叶子。
        std::uint32_t height = 0;
        // 树高字段（叶子为 0）。
        std::uint64_t parentId = 0;
        // 父页号，0 表示根。
        std::uint64_t parentGen = 0;
        // 父页代数。
        std::uint64_t leftId = 0;
        // 左兄弟页号。
        std::uint64_t leftGen = 0;
        // 左兄弟页代数。
        std::uint64_t rightId = 0;
        // 右兄弟页号。
        std::uint64_t rightGen = 0;
        // 右兄弟页代数。
        std::vector<IndexKey> keys;
        // 键列表。
        std::vector<RowRef> values;       // leaf
        // 叶子才用：键对应的行。
        std::vector<PageRef> children;    // internal
        // 内部节点才用：子页引用。
    };
    std::shared_ptr<PageFile> file_;
    // 页文件，用于枚举该索引占用的页。
    BufferPool& buffer_;
    // 缓冲池。
    std::uint64_t owner_;
    // 该索引的归属标识。
    std::size_t maxKeys_;
    // 节点键数上限。
    bool unique_;
    // 是否唯一索引。
    std::size_t minKeys_;
    // 节点键数下限，取上限的一半左右，用于判断下溢。
    mutable std::optional<PageRef> meta_;
    // 元页位置缓存；mutable 使 const 方法也能惰性填充。
    std::size_t count_ = 0;
    // 当前索引项数缓存。

    // 二进制编解码
    static std::vector<std::uint8_t> encodeValue(const Value& value);
    // 把一个键值编码成带类型标记的字节。
    static Value decodeValue(const std::vector<std::uint8_t>& record, std::size_t& offset);
    // 从字节流还原一个键值，并推进游标。
    static std::vector<std::uint8_t> nodeRecord(const Node& node);
    // 把节点序列化成页内记录。
    static Node nodeView(std::vector<std::uint8_t> record);
    // 把页内记录还原成节点。
    static std::vector<std::uint8_t> metaRecord(std::size_t maxKeys, bool unique, std::size_t size, PageRef root);
    // 序列化元页内容。
    static bool parseMeta(const std::vector<std::uint8_t>& record, std::size_t& maxKeys, bool& unique,
                          std::size_t& size, PageRef& root);
    // 解析元页内容，格式不符时返回 false。

    std::optional<PageRef> findMeta() const;
    // 在该索引占用的页里找出元页。
    PageRef requireMeta(PageRef& root, std::size_t& size) const;   // 读元页并返回其引用
    // 读取元页；顺带把根引用与大小通过出参返回。
    void writeMeta(PageRef root, std::size_t size);
    // 更新元页中的根与大小。
    Node readNode(PageRef ref) const;
    // 读取并解析一个节点页。
    void replaceNode(PageRef ref, const Node& node);
    // 用新内容整体覆盖一个节点页。
    template <typename Mutator>
    void mutateNode(PageRef ref, Mutator&& mutator);
    // 读—改—写三步封装，避免每处都重复样板代码。

    void insertInto(PageRef node, const IndexKey& key, RowRef row, bool isRoot,
                    std::optional<std::pair<IndexKey, PageRef>>& split,
                    std::optional<PageRef>& rootReplace);
    // 递归插入：自底向上分裂，必要时换根。
    bool eraseInto(PageRef node, const IndexKey& key, RowRef row, const PageRef& root,
                   std::optional<PageRef>& rootReplace, bool& underflow);
    // 递归删除：返回是否删除成功，并通过出参上报下溢。
    bool rebalanceChild(Node& n, std::size_t childIdx, PageRef childRef, bool nodeIsRoot);
    // 子节点下溢时先尝试向兄弟借位，借不到就合并。
    PageRef locateLeaf(PageRef root) const;
    // 走到最左叶子，范围扫描与校验的起点。
    PageRef liftLeaf(PageRef root, const IndexKey& key) const;
    // 按某个键走到对应的叶子。
    std::size_t childIndex(const std::vector<IndexKey>& keys, const IndexKey& key) const;
    // 内部节点里应该下钻的子节点下标。
    std::size_t lowerBound(const std::vector<IndexKey>& keys, const IndexKey& key) const;
    // 叶子内第一个不小于该键的位置。
    bool validateNode(PageRef ref, std::uint32_t expected, std::optional<const IndexKey*> left,
                      std::optional<const IndexKey*> right, std::size_t& leafDepth) const;
    // 递归校验：键在上下界之间、有序、叶子深度一致。
};

template <typename Mutator>
void PageBPlusTree::mutateNode(PageRef ref, Mutator&& mutator) {
// 模板实现放在头文件，调用方才能实例化。
    Node node = readNode(ref);
    // 先读出节点。
    mutator(node);
    // 交给调用方修改。
    replaceNode(ref, node);
    // 再整体写回。
}
}  // namespace minisql::storage
