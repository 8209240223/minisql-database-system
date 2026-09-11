#pragma once
#include "minisql/storage/page_file.hpp"
#include <memory>

namespace minisql::storage {
enum class ReplacementPolicy { LRU, FIFO };
struct BufferStats {
    std::uint64_t hits = 0, misses = 0, pageReads = 0, pageWrites = 0, ioErrors = 0;
    std::uint64_t stagedPageReads = 0, stagedPageWrites = 0;
};
struct Eviction {
    std::uint64_t sequence;
    ReplacementPolicy policy;
    PageRef page;
    bool dirty;
    std::string writeBack = "not-needed";
};
struct BufferFrame {
    SlottedPage page;
    std::size_t pins = 0;
    bool dirty = false;
    std::uint64_t loaded = 0, accessed = 0;
};
class PageGuard {
public:
    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&&) noexcept = default;
    PageGuard& operator=(PageGuard&& other) noexcept;
    ~PageGuard();
    const SlottedPage& page() const;
    SlotRef insert(std::span<const std::uint8_t> record);
    void erase(SlotRef ref);
private:
    friend class BufferPool;
    explicit PageGuard(std::shared_ptr<BufferFrame> frame);
    std::shared_ptr<BufferFrame> frame_;
};
class BufferPool {
public:
    BufferPool(std::shared_ptr<PageFile> file, std::size_t capacity, ReplacementPolicy policy);
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    PageGuard get(PageRef ref);
    PageRef allocate(std::uint64_t owner);
    void release(PageRef ref);
    void flush(PageRef ref);
    void flushAll();
    void beginWriteBatch();
    void rollbackWriteBatch();
    void commitWriteBatch();
    void setPolicy(ReplacementPolicy policy);
    void resetStats();
    // 页替换日志输出：非空路径时，每次淘汰都以追加方式写一行文本日志（命中统计仍走 stats()）。
    void setEvictionLog(std::filesystem::path path);
    const BufferStats& stats() const { return stats_; }
    const std::vector<Eviction>& evictions() const { return evictions_; }
    std::size_t size() const { return frames_.size(); }
    std::size_t capacity() const { return capacity_; }
    ReplacementPolicy policy() const { return policy_; }
    std::size_t pins(PageRef ref) const;
    std::size_t dirtyPages() const;
private:
    std::shared_ptr<PageFile> file_;
    std::size_t capacity_;
    ReplacementPolicy policy_;
    std::uint64_t sequence_ = 0;
    std::unordered_map<PageId, std::shared_ptr<BufferFrame>> frames_;
    BufferStats stats_;
    std::vector<Eviction> evictions_;
    std::filesystem::path evictionLogPath_;
    void makeRoom();
    void writeBack(BufferFrame& frame);
    void appendEvictionLog(const Eviction& event) const;
    SlottedPage readPage(PageRef ref);
    void requireUnpinned() const;
};
}
