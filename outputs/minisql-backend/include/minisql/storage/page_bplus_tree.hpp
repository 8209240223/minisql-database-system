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

// 页级结构校验输出（每页一条；页类型/height/keyCount/父/兄弟指针，用于工作台诊断命令）。
struct IndexPageInfo {
    PageRef page;
    bool leaf = true;
    std::uint32_t height = 0;
    std::size_t keyCount = 0;
    PageRef parent{0, 0};
    PageRef left{0, 0};
    PageRef right{0, 0};
};
struct IndexInspect {
    bool present = false;
    PageRef meta{0, 0};
    PageRef root{0, 0};
    std::size_t height = 0;          // 树高（叶深计数）
    std::size_t nodeCount = 0;       // 节点页数
    std::size_t leafCount = 0;
    std::size_t rowCount = 0;        // 叶链内总键数
    std::size_t leafChainLength = 0;
    bool rootReachable = false;      // 从根可完整遍历
    bool leafChainLinked = false;    // 叶链左右指针成链
    bool parentLinksValid = false;   // 所有子页父指针都与父页一致
    std::vector<std::string> problems;
    std::vector<IndexPageInfo> pages;
};

class PageBPlusTree {
public:
    PageBPlusTree(std::shared_ptr<PageFile> file, BufferPool& buffer, std::uint64_t owner,
                  std::size_t maxKeys, bool unique);
    PageBPlusTree(const PageBPlusTree&) = delete;
    PageBPlusTree& operator=(const PageBPlusTree&) = delete;

    bool exists() const;
    bool create();                       // 无元页时创建，返回是否新建
    bool insert(IndexKey key, RowRef row);
    bool erase(IndexKey key, RowRef row);   // 删除并触发页内借位/合并与根收缩
    std::vector<RowRef> search(const IndexKey& key) const;
    std::vector<RowRef> range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                              const std::optional<IndexKey>& upper, bool upperInclusive) const;
    std::size_t size() const;
    std::size_t height() const;
    std::size_t pageCount() const;
    bool validate() const;
    IndexInspect inspect() const;   // 页级结构校验：页类型/height/keyCount/父/兄弟/叶链/根可达

private:
    struct Node {
        bool leaf = true;
        std::uint32_t height = 0;
        std::uint64_t parentId = 0;
        std::uint64_t parentGen = 0;
        std::uint64_t leftId = 0;
        std::uint64_t leftGen = 0;
        std::uint64_t rightId = 0;
        std::uint64_t rightGen = 0;
        std::vector<IndexKey> keys;
        std::vector<RowRef> values;       // leaf
        std::vector<PageRef> children;    // internal
    };
    std::shared_ptr<PageFile> file_;
    BufferPool& buffer_;
    std::uint64_t owner_;
    std::size_t maxKeys_;
    bool unique_;
    std::size_t minKeys_;
    mutable std::optional<PageRef> meta_;
    std::size_t count_ = 0;

    // 二进制编解码
    static std::vector<std::uint8_t> encodeValue(const Value& value);
    static Value decodeValue(const std::vector<std::uint8_t>& record, std::size_t& offset);
    static std::vector<std::uint8_t> nodeRecord(const Node& node);
    static Node nodeView(std::vector<std::uint8_t> record);
    static std::vector<std::uint8_t> metaRecord(std::size_t maxKeys, bool unique, std::size_t size, PageRef root);
    static bool parseMeta(const std::vector<std::uint8_t>& record, std::size_t& maxKeys, bool& unique,
                          std::size_t& size, PageRef& root);

    std::optional<PageRef> findMeta() const;
    PageRef requireMeta(PageRef& root, std::size_t& size) const;   // 读元页并返回其引用
    void writeMeta(PageRef root, std::size_t size);
    Node readNode(PageRef ref) const;
    void replaceNode(PageRef ref, const Node& node);
    template <typename Mutator>
    void mutateNode(PageRef ref, Mutator&& mutator);

    void insertInto(PageRef node, const IndexKey& key, RowRef row, bool isRoot,
                    std::optional<std::pair<IndexKey, PageRef>>& split,
                    std::optional<PageRef>& rootReplace);
    bool eraseInto(PageRef node, const IndexKey& key, RowRef row, const PageRef& root,
                   std::optional<PageRef>& rootReplace, bool& underflow);
    bool rebalanceChild(Node& n, std::size_t childIdx, PageRef childRef, bool nodeIsRoot);
    PageRef locateLeaf(PageRef root) const;
    PageRef liftLeaf(PageRef root, const IndexKey& key) const;
    std::size_t childIndex(const std::vector<IndexKey>& keys, const IndexKey& key) const;
    std::size_t lowerBound(const std::vector<IndexKey>& keys, const IndexKey& key) const;
    bool validateNode(PageRef ref, std::uint32_t expected, std::optional<const IndexKey*> left,
                      std::optional<const IndexKey*> right, std::size_t& leafDepth) const;
};

template <typename Mutator>
void PageBPlusTree::mutateNode(PageRef ref, Mutator&& mutator) {
    Node node = readNode(ref);
    mutator(node);
    replaceNode(ref, node);
}
}  // namespace minisql::storage