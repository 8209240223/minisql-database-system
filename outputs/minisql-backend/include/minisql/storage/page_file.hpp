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
// 持久化检查点记录（随 .ckpt sidecar 落盘），记录 WAL 截止位置与恢复元数据。
struct CheckpointRecord {
    bool present = false;
    std::uint64_t walCutoffBytes = 0;   // 本次检查点吸收/截至的 WAL 字节（截止位置；未检查点的剩余日志）
    std::uint64_t dirtyWatermark = 0;   // 已落盘到主文件的页数上限（脏页水位/恢复起点）
    std::uint64_t catalogVersion = 0;   // 目录版本
    std::uint64_t indexVersion = 0;     // 索引版本
    std::uint64_t committedSequence = 0; // 检查点时的提交序号（LSN 语义）
    std::uint64_t timestampMs = 0;      // 检查点时间戳
};
struct CheckpointOptions {
    std::uint64_t catalogVersion = 0;   // 目录版本（目录/索引版本由上层语义维护）
    std::uint64_t indexVersion = 0;
};
class PageFile {
public:
    using CommitObserver = std::function<void(std::string_view)>;
    explicit PageFile(const std::filesystem::path& path, CommitObserver observer = {});
    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    PageRef allocate(std::uint64_t owner);
    SlottedPage read(PageRef ref);
    void write(const SlottedPage& page);
    void release(PageRef ref);
    void flush();
    void beginWriteBatch();
    void rollbackWriteBatch();
    void commitWriteBatch();
    void checkpoint(const CheckpointOptions& options = {});
    void requireHealthy() const;
    bool writeBatchActive() const { return batch_ != nullptr; }
    bool hasStagedPage(PageId id) const { return batch_ && batch_->pages.contains(id); }
    std::size_t stagedPageCount() const { return batch_ ? batch_->pages.size() : 0; }
    std::size_t allocatedPages() const { return active_.size(); }
    std::uint64_t walBytes() const;
    std::uint64_t lastCommitWalBytes() const { return lastCommitWalBytes_; }
    std::uint64_t committedSequence() const { return committedSequence_; }
    std::uint64_t dirtyWatermark() const { return dirtyWatermark_; }
    const CheckpointRecord& checkpointRecord() const { return checkpointRecord_; }
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
    std::uint64_t dirtyWatermark_ = 1;
    CheckpointRecord checkpointRecord_;
    bool failed_ = false;
    PageIoStats ioStats_;
    std::unordered_map<PageId, std::uint64_t> active_;
    std::unordered_map<PageId, std::uint64_t> owners_;
    std::vector<PageRef> free_;
    struct WriteBatch {
        std::uint64_t count;
        std::unordered_map<PageId, std::uint64_t> active, owners;
        std::vector<PageRef> free;
        std::unordered_map<PageId, PageBytes> pages;
        std::uint64_t startLsn = 0;   // 批次在 WAL 中的起始字节偏移，回滚时截断其后未提交的预备扩展
        bool published = false;
    };
    std::unique_ptr<WriteBatch> batch_;
    PageBytes readRaw(PageId id);
    void writeRaw(PageId id, const PageBytes& bytes);
    void writeDiskRaw(PageId id, const PageBytes& bytes);
    void ensureIdentity();
    void recoverJournal(bool recovering);
    void applyExtent(const std::map<PageId, PageBytes>& pages, std::uint64_t finalCount, bool recovering);
    void checkpointJournal();
    void writeCheckpointRecord();
    void loadCheckpointRecord();
    void notify(std::string_view stage);
    void writeHeader();
    void requireActive(PageRef ref) const;
};
}
