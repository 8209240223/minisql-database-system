#pragma once
#include "minisql/storage/heap.hpp"
#include <memory>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace minisql::storage {
struct IndexKey {
// 索引键：由若干列的值组成，支持复合索引。
    std::vector<Value> values;
    // 各列的值，按索引列顺序排列。
    bool operator==(const IndexKey& other) const { return compare(*this, other) == 0; }
    // 相等即比较结果为 0。
    static int compare(const IndexKey& left, const IndexKey& right);
    // 逐列比较；返回负数、零或正数表示小于、等于、大于。
};
struct IndexEntry { IndexKey key; RowRef row; };
// 索引项：键加上它指向的行。

class BPlusTree {
// 内存版 B+ 树：叶子存放数据并串成链表，内部节点只存分隔键。
public:
    explicit BPlusTree(std::size_t maxKeys = 64, bool unique = false);
    // 构造：maxKeys 是单个节点的键数上限，unique 表示是否要求键唯一。
    BPlusTree(const BPlusTree&) = delete;
    // 树持有节点所有权，禁止拷贝。
    BPlusTree& operator=(const BPlusTree&) = delete;
    // 同样禁止拷贝赋值。

    bool insert(IndexKey key, RowRef row);
    // 插入一个键；唯一索引遇到重复键时返回 false，不再抛异常。
    std::vector<RowRef> search(const IndexKey& key) const;
    // 等值查找，返回所有匹配的行（非唯一索引可能有多行）。
    std::vector<RowRef> range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                              const std::optional<IndexKey>& upper, bool upperInclusive) const;
    // 范围查找：上下界都可以缺省，是否包含端点由两个布尔决定。
    std::size_t size() const { return size_; }
    // 索引项总数。
    std::size_t height() const;
    // 树高，叶子层为 1；性能曲线测试会统计它。
    bool validate() const;
    // 结构自检：键有序、节点平衡、叶链完整。
    void save(const std::filesystem::path& path, const std::string& fingerprint) const;
    // 把整棵树写成 JSON 快照，附带指纹用于校验它属于哪张表。
    void load(const std::filesystem::path& path, const std::string& expectedFingerprint);
    // 从快照文件恢复，指纹不符则拒绝。
    std::string dump(const std::string& fingerprint) const;
    // 序列化成字符串，save 与索引页存储都调用它。
    void restore(const std::string& bytes, const std::string& expectedFingerprint);
    // 从字符串恢复。
private:
    struct Node {
    // 节点：叶子与内部节点共用同一个结构。
        bool leaf = true;
        // 是否叶子。
        std::vector<IndexKey> keys;
        // 键列表。
        std::vector<RowRef> values;
        // 叶子才用：每个键对应的行。
        std::vector<std::unique_ptr<Node>> children;
        // 内部节点才用：子指针，数量恒为键数加一。
        Node* next = nullptr;
        // 叶子才用：指向下一个叶子的链表指针，范围扫描靠它。
    };
    struct Split { IndexKey key; std::unique_ptr<Node> right; };
    // 分裂结果：被提升的键与新建的右兄弟节点。
    std::unique_ptr<Node> root_;
    // 根节点。
    std::size_t maxKeys_;
    // 单节点键数上限。
    bool unique_;
    // 是否唯一索引。
    std::size_t size_ = 0;
    // 索引项总数。

    std::optional<Split> insert(Node& node, IndexKey key, RowRef row);
    // 递归插入：返回空表示无需分裂，否则返回分裂结果交给父节点。
    const Node* findLeaf(const IndexKey& key) const;
    // 从根走到包含该键的叶子。
    void collect(const Node& node, const std::optional<IndexKey>& lower, bool lowerInclusive,
                 const std::optional<IndexKey>& upper, bool upperInclusive, std::vector<RowRef>& rows) const;
    // 递归收集落在范围内的行。
    bool validateNode(const Node& node, std::size_t depth, std::size_t& leafDepth) const;
    // 递归校验：键有序、子节点数正确、所有叶子深度一致。
};
}
