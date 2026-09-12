#pragma once
#include "minisql/storage/heap.hpp"
#include <memory>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace minisql::storage {
struct IndexKey {
    std::vector<Value> values;
    bool operator==(const IndexKey& other) const { return compare(*this, other) == 0; }
    static int compare(const IndexKey& left, const IndexKey& right);
};
struct IndexEntry { IndexKey key; RowRef row; };

class BPlusTree {
public:
    explicit BPlusTree(std::size_t maxKeys = 64, bool unique = false);
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    bool insert(IndexKey key, RowRef row);
    bool erase(const IndexKey& key, RowRef row);
    void reset();   // 清空所有节点，供索引重建复用同一实例
    std::vector<RowRef> search(const IndexKey& key) const;
    std::vector<RowRef> range(const std::optional<IndexKey>& lower, bool lowerInclusive,
                              const std::optional<IndexKey>& upper, bool upperInclusive) const;
    std::size_t size() const { return size_; }
    std::size_t height() const;
    bool validate() const;
    void save(const std::filesystem::path& path, const std::string& fingerprint) const;
    void load(const std::filesystem::path& path, const std::string& expectedFingerprint);
    std::string dump(const std::string& fingerprint) const;
    void restore(const std::string& bytes, const std::string& expectedFingerprint);
private:
    struct Node {
        bool leaf = true;
        std::vector<IndexKey> keys;
        std::vector<RowRef> values;
        std::vector<std::unique_ptr<Node>> children;
        Node* next = nullptr;
    };
    struct Split { IndexKey key; std::unique_ptr<Node> right; };
    std::unique_ptr<Node> root_;
    std::size_t maxKeys_;
    bool unique_;
    std::size_t size_ = 0;

    std::optional<Split> insert(Node& node, IndexKey key, RowRef row);
    const Node* findLeaf(const IndexKey& key) const;
    void collect(const Node& node, const std::optional<IndexKey>& lower, bool lowerInclusive,
                 const std::optional<IndexKey>& upper, bool upperInclusive, std::vector<RowRef>& rows) const;
    void collectEntries(const Node& node, std::vector<IndexEntry>& entries) const;
    bool validateNode(const Node& node, std::size_t depth, std::size_t& leafDepth) const;
};
}
