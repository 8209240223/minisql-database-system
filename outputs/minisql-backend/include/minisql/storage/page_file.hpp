#pragma once
#include "minisql/storage/page.hpp"
#include "minisql/storage/file_io.hpp"
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <map>
#include <memory>
#include <functional>
#include <string_view>

namespace minisql::storage {
struct PageRef { PageId id; std::uint64_t generation; };
struct PageIoStats { std::uint64_t reads = 0, writes = 0, errors = 0; };
// WAL 记录级统计：逻辑 LSN 链、事务/记录类型计数、归档与双写状态。
struct WalStatistics {
    std::uint64_t walBytes = 0;            // 期刊当前字节长度
    std::uint64_t nextLsn = 0;             // 下一个可分配的逻辑 LSN
    std::uint64_t lastExtentLsn = 0;       // 最近一个已解析扩展的逻辑 LSN
    std::uint64_t lastCheckpointLsn = 0;   // 最近一次检查点持久化的 LSN 水位
    std::uint64_t committedSequence = 0;   // 已提交扩展计数（提交序号）
    std::uint64_t dirtyWatermark = 0;      // 脏页水位（已落盘页数上限）
    std::uint64_t committedExtents = 0;    // 本次打开后提交的扩展数
    std::uint64_t abortedExtents = 0;      // 本次打开后写入的撤销（Abort）扩展数
    std::uint64_t recordedPages = 0;       // 已写入期刊的页记录数
    std::uint64_t trackedPages = 0;        // 页级 LSN 链长度
    std::uint64_t archivedBytes = 0;       // 已归档的期刊字节
    std::uint64_t archiveSegments = 0;     // 归档段数
    std::uint64_t pendingCommitBytes = 0;  // 尚未同步到稳定存储的提交字节
    bool groupCommit = false;              // 批量提交（延迟 fsync）模式
    bool doubleWrite = false;              // 双写缓冲已启用
    bool fuzzyCheckpoint = false;          // 模糊检查点已启用
};
// 日志记录类型（扩展头 recordType）。
enum class WalRecordType : std::uint32_t { Redo = 1, Commit = 2, Abort = 3 };
// 持久化检查点记录（随 .ckpt sidecar 落盘），记录 WAL 截止位置与恢复元数据。
struct CheckpointRecord {
    bool present = false;
    std::uint64_t walCutoffBytes = 0;   // 本次检查点吸收/截至的 WAL 字节（截止位置；未检查点的剩余日志）
    std::uint64_t dirtyWatermark = 0;   // 已落盘到主文件的页数上限（脏页水位/恢复起点）
    std::uint64_t catalogVersion = 0;   // 目录版本
    std::uint64_t indexVersion = 0;     // 索引版本
    std::uint64_t committedSequence = 0; // 检查点时的提交序号（LSN 语义）
    std::uint64_t timestampMs = 0;      // 检查点时间戳
    std::uint64_t walLsn = 0;           // 检查点持久化的逻辑 LSN 水位（截断后仍保证 LSN 单调）
    std::uint64_t checkpointBeginLsn = 0;  // 模糊检查点的起始 LSN（0 = 非模糊）
    std::uint64_t checkpointEndLsn = 0;    // 模糊检查点的结束 LSN
    std::uint64_t archivedBytes = 0;       // 已归档期刊字节
    std::uint64_t archiveSegments = 0;     // 归档段数
};
struct CheckpointOptions {
    std::uint64_t catalogVersion = 0;   // 目录版本（目录/索引版本由上层语义维护）
    std::uint64_t indexVersion = 0;
    bool fuzzy = false;                 // 模糊检查点：记录起始/结束 LSN 与截止位置，不截断日志
    bool archive = false;               // 回收前把日志前缀归档到 .wal.archive.N
};
struct PageFileSavepoint {
    std::uint64_t count = 1;
    std::unordered_map<PageId, std::uint64_t> active;
    std::unordered_map<PageId, std::uint64_t> owners;
    std::vector<PageRef> free;
    std::unordered_map<PageId, PageBytes> pages;
    bool published = false;
};
class PageFile {
public:
    using CommitObserver = std::function<void(std::string_view)>;
    explicit PageFile(const std::filesystem::path& path, CommitObserver observer = {});
    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    ~PageFile();
    PageRef allocate(std::uint64_t owner);
    SlottedPage read(PageRef ref);
    void write(const SlottedPage& page);
    void release(PageRef ref);
    void flush();
    void beginWriteBatch();
    void rollbackWriteBatch();
    void commitWriteBatch();
    PageFileSavepoint savepoint() const;
    void restoreSavepoint(const PageFileSavepoint& snapshot);
    void checkpoint(const CheckpointOptions& options = {});
    // group commit：把多个提交的日志落盘合并为一次 fsync，并在同一次批次中把
    // 其数据页应用到主文件；返回本次真正同步/应用的范围描述。
    void syncJournalGroup();
    bool groupCommitEnabled() const noexcept { return groupCommit_; }
    void requireHealthy() const;
    bool writeBatchActive() const { return batch_ != nullptr; }
    bool hasStagedPage(PageId id) const { return batch_ && batch_->pages.contains(id); }
    std::size_t stagedPageCount() const { return batch_ ? batch_->pages.size() : 0; }
    std::size_t allocatedPages() const { return active_.size(); }
    std::uint64_t walBytes() const;
    // 记录级 WAL 统计（逻辑 LSN、事务/记录计数、归档与双写状态）。
    WalStatistics walStatistics() const;
    std::uint64_t lastCommitWalBytes() const { return lastCommitWalBytes_; }
    std::uint64_t committedSequence() const { return committedSequence_; }
    std::uint64_t dirtyWatermark() const { return dirtyWatermark_; }
    const CheckpointRecord& checkpointRecord() const { return checkpointRecord_; }
    void copyTo(const std::filesystem::path& destination);
    const std::filesystem::path& path() const { return path_; }
    const PageIoStats& ioStats() const { return ioStats_; }
    void resetIoStats() { ioStats_ = {}; }
    std::vector<PageRef> pagesFor(std::uint64_t owner) const;
private:
    std::filesystem::path path_;
    std::unique_ptr<ExclusiveFileLock> lock_;
    std::fstream file_;
    std::array<std::uint64_t, 2> identity_{};
    CommitObserver observer_;
    std::uint64_t count_ = 1;
    std::uint64_t lastCommitWalBytes_ = 0;
    std::uint64_t committedSequence_ = 0;
    std::uint64_t txSequence_ = 0;      // 事务 id 分配器：随每次 beginWriteBatch 递增，仅用于 WAL 头标识
    std::uint64_t dirtyWatermark_ = 1;
    std::uint64_t nextLsn_ = 1;         // 逻辑 LSN 分配器（单调，跨日志截断保持）
    std::uint64_t lastExtentLsn_ = 0;   // 最近一个扩展的逻辑 LSN（全局 prevLsn 链尾）
    std::uint64_t committedExtents_ = 0;  // 本次打开后提交的扩展数
    std::uint64_t abortedExtents_ = 0;    // 本次打开后写入的撤销（Abort）扩展数
    std::uint64_t recordedPages_ = 0;     // 本次打开后写入期刊的页记录数
    std::unordered_map<std::uint64_t, std::uint64_t> transactionLsn_;  // txId -> 最近扩展 LSN（事务内 prevLsn/undoNextLsn）
    std::unordered_map<PageId, std::uint64_t> pageLsn_;               // 页 -> 最近一次页记录 LSN
    CheckpointRecord checkpointRecord_;
    bool failed_ = false;
    PageIoStats ioStats_;
    bool doubleWrite_ = false;      // 双写缓冲已启用（torn-page 防护）
    bool groupCommit_ = false;      // group commit：延迟 fsync 与数据页应用
    // 已提交但尚未 fsync/应用的扩展（group commit 模式下暂存）。
    struct PendingExtent {
        std::map<PageId, PageBytes> pages;
        std::uint64_t finalCount = 0;
    };
    std::vector<PendingExtent> pendingExtents_;
    std::unordered_map<PageId, PageBytes> pendingPages_;   // 未应用扩展的读覆盖
    std::uint64_t pendingBytes_ = 0;
    void recycleJournal(bool archive, std::uint64_t cutoff);
    std::unordered_map<PageId, std::uint64_t> active_;
    std::unordered_map<PageId, std::uint64_t> owners_;
    std::vector<PageRef> free_;
    struct WriteBatch {
        std::uint64_t count;
        std::unordered_map<PageId, std::uint64_t> active, owners;
        std::vector<PageRef> free;
        std::unordered_map<PageId, PageBytes> pages;
        std::uint64_t txId = 0;       // 事务 id：本批次唯一递增标识，随 WAL 扩展头持久化
        std::uint64_t startLsn = 0;   // 批次在 WAL 中的起始字节偏移，回滚时截断其后未提交的预备扩展
        std::uint64_t prevLsn = 0;    // 本批次之前的全局链尾逻辑 LSN（记录级 prevLsn）
        std::uint64_t extentLsn = 0;  // 本批次扩展的逻辑 LSN
        bool touched = false;         // 本批次是否曾暂存过页（决定回滚是否写 Abort 记录）
        bool published = false;
    };
    std::unique_ptr<WriteBatch> batch_;
    PageBytes readRaw(PageId id);
    void writeRaw(PageId id, const PageBytes& bytes);
    void writeDiskRaw(PageId id, const PageBytes& bytes);
    void ensureIdentity();
    void recoverJournal(bool recovering);
    // 双写缓冲（torn-page 防护，MINISQL_DOUBLEWRITE=1 时启用）：
    // 数据页在写主文件前先落入 .dwb 槽位并同步，主文件同步成功后再释放槽位；
    // 打开时用仍有效的槽位修复未完成页写。
    void recoverDoubleWrite();
    void stageDoubleWrite(const std::map<PageId, PageBytes>& pages);
    void clearDoubleWrite();
    void applyExtent(const std::map<PageId, PageBytes>& pages, std::uint64_t finalCount, bool recovering);
    void checkpointJournal();
    void writeCheckpointRecord();
    void loadCheckpointRecord();
    void notify(std::string_view stage, bool allowCrash = true);
    void writeHeader();
    void requireActive(PageRef ref) const;
};
}
