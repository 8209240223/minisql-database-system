#pragma once
#include "minisql/storage/page_file.hpp"
#include <memory>

namespace minisql::storage {
enum class ReplacementPolicy { LRU, FIFO, CLOCK };
// 缓存淘汰策略：LRU 淘汰最久未访问的页；FIFO 淘汰最早装入的页；
// CLOCK（二次机会）用环形指针扫描，引用位为 1 的给一次机会并清位，为 0 的淘汰。
struct BufferStats {
// 缓存运行统计，供 statistics 命令输出。
    std::uint64_t hits = 0, misses = 0, pageReads = 0, pageWrites = 0, ioErrors = 0;
    // 依次为：命中次数、未命中次数、磁盘读次数、磁盘写次数、IO 错误次数。
    std::uint64_t stagedPageReads = 0, stagedPageWrites = 0;
    std::uint64_t clockSweeps = 0, clockSecondChances = 0;
    // CLOCK 专用：指针扫过的帧数（扫描代价）与给出二次机会的次数。
    // 这两项让"新算法与 LRU 的差异"可以被测量，而不是只写在报告里。
    // 写批次期间从暂存区读写的次数，与真正落盘区分开。
};
struct Eviction {
// 一次淘汰事件的记录。
    std::uint64_t sequence;
    // 事件序号，按发生顺序递增。
    ReplacementPolicy policy;
    // 当时使用的淘汰策略。
    PageRef page;
    // 被淘汰的页（页号加代数）。
    bool dirty;
    // 淘汰时是否为脏页。
    std::string writeBack = "not-needed";
    // 写回结果：不需要写回、已写盘、进入暂存、或写失败。
};
struct BufferFrame {
// 缓存帧：内存中容纳一页及其管理信息。
    SlottedPage page;
    // 页内容本身。
    std::size_t pins = 0;
    // 当前有多少个使用者持有这一页；非零时不允许淘汰。
    bool dirty = false;
    // 页是否被修改过但还没写回。
    std::uint64_t loaded = 0, accessed = 0;
    bool referenced = false;
    // CLOCK 的引用位：本页被访问过就置 1；淘汰扫描时据此决定"给二次机会"还是淘汰。
    // 装入顺序号与最近访问顺序号，分别供 FIFO 与 LRU 使用。
};
class PageGuard {
// 页面使用凭证：持有期间自动 pin 住这一页，析构时自动解除。
public:
    PageGuard(const PageGuard&) = delete;
    // 禁止拷贝，避免 pin 计数被复制而失控。
    PageGuard& operator=(const PageGuard&) = delete;
    // 同样禁止拷贝赋值。
    PageGuard(PageGuard&&) noexcept = default;
    // 允许移动，所有权可以转移但同一时刻只有一处持有。
    PageGuard& operator=(PageGuard&& other) noexcept;
    // 移动赋值需要先把原有引用的 pin 减掉。
    ~PageGuard();
    // 析构时减少 pin 计数，页因此可以被淘汰。
    const SlottedPage& page() const;
    // 读取页内容。
    SlotRef insert(std::span<const std::uint8_t> record);
    // 往页里插入记录，并把该页标记为脏页。
    void erase(SlotRef ref);
    // 从页里删除记录，同样标记脏页。
private:
    friend class BufferPool;
    // 只有缓冲池能构造它，保证 pin 计数由内部维护。
    explicit PageGuard(std::shared_ptr<BufferFrame> frame);
    // 私有构造，构造时 pin 加一。
    std::shared_ptr<BufferFrame> frame_;
    // 指向对应的缓存帧。
};
class BufferPool {
// 缓冲池：在内存里缓存若干页，供上层反复访问而不必每次读磁盘。
public:
    BufferPool(std::shared_ptr<PageFile> file, std::size_t capacity, ReplacementPolicy policy);
    // 构造：指定底层页文件、缓存容量（多少页）与淘汰策略。
    BufferPool(const BufferPool&) = delete;
    // 缓冲池持有缓存状态，禁止拷贝。
    BufferPool& operator=(const BufferPool&) = delete;
    // 同样禁止拷贝赋值。
    PageGuard get(PageRef ref);
    // 取页：命中直接返回，未命中则先腾空间再读入磁盘。
    PageRef allocate(std::uint64_t owner);
    // 分配一张新页，需要腾出缓存空间。
    void release(PageRef ref);
    // 释放页：先确认没有被 pin，再从缓存移除并交给页文件回收。
    void flush(PageRef ref);
    // 把指定页写回磁盘。
    void flushAll();
    // 把所有脏页写回磁盘，写批次收尾与检查点前都要调用。
    void beginWriteBatch();
    // 开始一个写批次：先刷干净缓存，再让页文件进入暂存模式。
    void rollbackWriteBatch();
    // 回滚写批次：撤销暂存并清空缓存，避免旧内容被再次写回。
    void restoreSavepoint(const PageFileSavepoint& snapshot);
    void commitWriteBatch();
    // 提交写批次：先把缓存刷到暂存区，再让页文件正式提交。
    void setPolicy(ReplacementPolicy policy);
    // 切换淘汰策略，切换时先刷盘并清空缓存。
    void resetStats();
    // 把统计与淘汰记录清零。
    // 页替换日志输出：非空路径时，每次淘汰都以追加方式写一行文本日志（命中统计仍走 stats()）。
    // 页替换日志输出：非空路径时，每次淘汰都以追加方式写一行文本日志（命中统计仍走 stats()）。
    void setEvictionLog(std::filesystem::path path);
    // 设置替换日志文件路径，空路径表示关闭。
    const BufferStats& stats() const { return stats_; }
    // 取统计信息。
    const std::vector<Eviction>& evictions() const { return evictions_; }
    // 取淘汰事件列表。
    std::size_t size() const { return frames_.size(); }
    // 当前驻留的页数。
    std::size_t capacity() const { return capacity_; }
    // 缓存容量上限。
    ReplacementPolicy policy() const { return policy_; }
    // 当前淘汰策略。
    std::size_t pins(PageRef ref) const;
    // 查询某页被 pin 的次数。
    std::size_t dirtyPages() const;
    // 统计当前脏页数量，自动检查点会用到。
private:
    std::shared_ptr<PageFile> file_;
    // 底层页文件。
    std::size_t capacity_;
    // 缓存容量。
    ReplacementPolicy policy_;
    // 淘汰策略。
    std::uint64_t sequence_ = 0;
    // 全局递增序号，用来给访问与装入排序。
    std::unordered_map<PageId, std::shared_ptr<BufferFrame>> frames_;
    // 页号到缓存帧的映射表，命中判定就是查它。
    BufferStats stats_;
    // 统计信息。
    std::vector<Eviction> evictions_;
    // 淘汰事件历史。
    std::filesystem::path evictionLogPath_;
    // 替换日志路径，为空表示不写日志。
    void makeRoom();
    std::vector<PageId> clockOrder_;
    // CLOCK 的环形顺序：按装入先后排列当前驻留的页号。
    std::size_t clockHand_ = 0;
    // CLOCK 的环形指针：指向下一次淘汰扫描的起点。
    PageId clockEvict();
    // CLOCK 选 victim：环形扫描，引用位为 1 的给一次机会并清位，为 0 的淘汰。
    // 缓存满时腾出一个位置，必要时先把脏页写回。
    void writeBack(BufferFrame& frame);
    // 把一帧写回磁盘并清掉脏标记。
    void appendEvictionLog(const Eviction& event) const;
    // 把一条淘汰事件追加到日志文件。
    SlottedPage readPage(PageRef ref);
    // 从页文件读取一页，并累计读写统计。
    void requireUnpinned() const;
    // 校验当前没有页被 pin，否则整体操作不安全。
};
}
