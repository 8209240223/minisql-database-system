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
// 页引用：页号加代数；代数变化说明这一页被回收后重新分配过。
struct PageIoStats { std::uint64_t reads = 0, writes = 0, errors = 0; };
// 页文件层的读写与错误计数。
// 持久化检查点记录（随 .ckpt sidecar 落盘），记录 WAL 截止位置与恢复元数据。
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
// 检查点元数据：告诉恢复流程从哪里开始重做，以及当时的版本信息。
    bool present = false;
    // 是否已经存在一条检查点记录。
    std::uint64_t walCutoffBytes = 0;   // 本次检查点吸收/截至的 WAL 字节（截止位置；未检查点的剩余日志）
    // WAL 中已被落盘数据覆盖的前缀长度，恢复时从这里往后重做。
    std::uint64_t dirtyWatermark = 0;   // 已落盘到主文件的页数上限（脏页水位/恢复起点）
    // 主文件中已经确定有效的页数上限。
    std::uint64_t catalogVersion = 0;   // 目录版本
    // 检查点时的目录版本号。
    std::uint64_t indexVersion = 0;     // 索引版本
    // 检查点时的索引版本号。
    std::uint64_t committedSequence = 0; // 检查点时的提交序号（LSN 语义）
    // 已提交事务的序号，恢复后要保证不回退。
    std::uint64_t timestampMs = 0;      // 检查点时间戳
    // 检查点发生时间，便于排查。
    std::uint64_t walLsn = 0;           // 检查点持久化的逻辑 LSN 水位（截断后仍保证 LSN 单调）
    std::uint64_t checkpointBeginLsn = 0;  // 模糊检查点的起始 LSN（0 = 非模糊）
    std::uint64_t checkpointEndLsn = 0;    // 模糊检查点的结束 LSN
    std::uint64_t archivedBytes = 0;       // 已归档期刊字节
    std::uint64_t archiveSegments = 0;     // 归档段数
};
struct CheckpointOptions {
// 执行检查点时由上层传入的版本信息。
    std::uint64_t catalogVersion = 0;   // 目录版本（目录/索引版本由上层语义维护）
    // 目录版本。
    std::uint64_t indexVersion = 0;
    // 索引版本。
    bool fuzzy = false;                 // 模糊检查点：记录起始/结束 LSN 与截止位置，不截断日志
    bool archive = false;               // 回收前把日志前缀归档到 .wal.archive.N
};
struct PageFileSavepoint {
// 保存点：记录页文件的全部内存状态，供回滚还原。
    std::uint64_t count = 1;
    // 当时的页总数。
    std::unordered_map<PageId, std::uint64_t> active;
    // 有效页及其代数。
    std::unordered_map<PageId, std::uint64_t> owners;
    // 有效页的归属者。
    std::vector<PageRef> free;
    // 空闲页列表。
    std::unordered_map<PageId, PageBytes> pages;
    // 写批次中尚未提交的页内容。
    bool published = false;
    // 该保存点建立之后是否已经越过提交点。
};
class PageFile {
// 页文件：把数据库文件当作定长页的数组来读写，并负责空闲页管理与崩溃恢复。
public:
    using CommitObserver = std::function<void(std::string_view)>;
    // 提交阶段回调类型，测试用它注入崩溃点。
    explicit PageFile(const std::filesystem::path& path, CommitObserver observer = {});
    // 构造：加锁、打开文件、必要时重做日志并重建内存里的页表。
    PageFile(const PageFile&) = delete;
    // 页文件持有文件句柄与锁，禁止拷贝。
    PageFile& operator=(const PageFile&) = delete;
    // 同样禁止拷贝赋值。
    ~PageFile();
    PageRef allocate(std::uint64_t owner);
    // 分配一页：优先复用空闲页，否则在文件末尾追加。
    SlottedPage read(PageRef ref);
    // 读取一页并校验页号、代数与归属。
    void write(const SlottedPage& page);
    // 写入一页；写批次期间只进暂存区。
    void release(PageRef ref);
    // 释放一页：写成空闲页标记并加入空闲列表。
    void flush();
    // 把文件缓冲刷出去。
    void beginWriteBatch();
    // 开始写批次：建立快照，之后的写只进内存。
    void rollbackWriteBatch();
    // 回滚写批次：截断未提交的日志扩展并还原内存快照。
    void commitWriteBatch();
    // 提交写批次：先写日志与提交标记，再落盘数据页。
    PageFileSavepoint savepoint() const;
    // 取当前状态的保存点。
    void restoreSavepoint(const PageFileSavepoint& snapshot);
    // 还原到某个保存点。
    void checkpoint(const CheckpointOptions& options = {});
    // 执行检查点：截断日志并写入检查点记录。
    // group commit：把多个提交的日志落盘合并为一次 fsync，并在同一次批次中把
    // 其数据页应用到主文件；返回本次真正同步/应用的范围描述。
    void syncJournalGroup();
    bool groupCommitEnabled() const noexcept { return groupCommit_; }
    void requireHealthy() const;
    // 校验页文件没有处于故障状态。
    bool writeBatchActive() const { return batch_ != nullptr; }
    // 是否正在写批次中。
    bool hasStagedPage(PageId id) const { return batch_ && batch_->pages.contains(id); }
    // 某页是否在暂存区里被改过。
    std::size_t stagedPageCount() const { return batch_ ? batch_->pages.size() : 0; }
    // 暂存页数量。
    std::size_t allocatedPages() const { return active_.size(); }
    // 当前有效页数量。
    std::uint64_t walBytes() const;
    // 取日志文件当前大小。
    // 记录级 WAL 统计（逻辑 LSN、事务/记录计数、归档与双写状态）。
    WalStatistics walStatistics() const;
    std::uint64_t lastCommitWalBytes() const { return lastCommitWalBytes_; }
    // 上一次提交写了多少字节日志。
    std::uint64_t committedSequence() const { return committedSequence_; }
    // 当前已提交序号。
    std::uint64_t dirtyWatermark() const { return dirtyWatermark_; }
    // 当前脏页水位。
    const CheckpointRecord& checkpointRecord() const { return checkpointRecord_; }
    // 取检查点记录。
    void copyTo(const std::filesystem::path& destination);
    // 复制数据库文件及其 sidecar，用于备份。
    const std::filesystem::path& path() const { return path_; }
    // 数据库文件路径。
    const PageIoStats& ioStats() const { return ioStats_; }
    // 取 IO 统计。
    void resetIoStats() { ioStats_ = {}; }
    // 清零 IO 统计。
    std::vector<PageRef> pagesFor(std::uint64_t owner) const;
    // 列出某张表（或索引）占用的全部页，按页号排序。
private:
    std::filesystem::path path_;
    // 数据库文件路径。
    std::unique_ptr<ExclusiveFileLock> lock_;
    // 独占锁，进程退出自动释放。
    std::fstream file_;
    // 文件流。
    std::array<std::uint64_t, 2> identity_{};
    // 数据库身份标识，用于防止日志与数据库文件错配。
    CommitObserver observer_;
    // 提交阶段观察者。
    std::uint64_t count_ = 1;
    // 页总数，第 0 页是文件头。
    std::uint64_t lastCommitWalBytes_ = 0;
    // 上次提交的日志字节数。
    std::uint64_t committedSequence_ = 0;
    // 已提交序号。
    std::uint64_t txSequence_ = 0;      // 事务 id 分配器：随每次 beginWriteBatch 递增，仅用于 WAL 头标识
    // 事务号分配器。
    std::uint64_t dirtyWatermark_ = 1;
    // 脏页水位。
    std::uint64_t nextLsn_ = 1;         // 逻辑 LSN 分配器（单调，跨日志截断保持）
    std::uint64_t lastExtentLsn_ = 0;   // 最近一个扩展的逻辑 LSN（全局 prevLsn 链尾）
    std::uint64_t committedExtents_ = 0;  // 本次打开后提交的扩展数
    std::uint64_t abortedExtents_ = 0;    // 本次打开后写入的撤销（Abort）扩展数
    std::uint64_t recordedPages_ = 0;     // 本次打开后写入期刊的页记录数
    std::unordered_map<std::uint64_t, std::uint64_t> transactionLsn_;  // txId -> 最近扩展 LSN（事务内 prevLsn/undoNextLsn）
    std::unordered_map<PageId, std::uint64_t> pageLsn_;               // 页 -> 最近一次页记录 LSN
    CheckpointRecord checkpointRecord_;
    // 检查点记录。
    bool failed_ = false;
    // 发生 IO 错误后置位，之后禁止继续使用。
    PageIoStats ioStats_;
    // IO 统计。
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
    // 有效页到其代数的映射。
    std::unordered_map<PageId, std::uint64_t> owners_;
    // 有效页到归属者的映射。
    std::vector<PageRef> free_;
    // 空闲页列表。
    struct WriteBatch {
    // 写批次的快照与暂存内容。
        std::uint64_t count;
        // 批次开始时的页总数。
        std::unordered_map<PageId, std::uint64_t> active, owners;
        // 批次开始时的有效页与归属者。
        std::vector<PageRef> free;
        // 批次开始时的空闲页列表。
        std::unordered_map<PageId, PageBytes> pages;
        // 批次内被修改页的新内容。
        std::uint64_t txId = 0;       // 事务 id：本批次唯一递增标识，随 WAL 扩展头持久化
        // 批次事务号。
        std::uint64_t startLsn = 0;   // 批次在 WAL 中的起始字节偏移，回滚时截断其后未提交的预备扩展
        // 批次开始时日志的长度。
        std::uint64_t prevLsn = 0;    // 本批次之前的全局链尾逻辑 LSN（记录级 prevLsn）
        std::uint64_t extentLsn = 0;  // 本批次扩展的逻辑 LSN
        bool touched = false;         // 本批次是否曾暂存过页（决定回滚是否写 Abort 记录）
        bool published = false;
        // 是否已经写过提交标记。
    };
    std::unique_ptr<WriteBatch> batch_;
    // 当前写批次；为空表示不在批次中。
    PageBytes readRaw(PageId id);
    // 读取原始页字节：优先取暂存区，其次读文件。
    void writeRaw(PageId id, const PageBytes& bytes);
    // 写原始页字节：批次内进暂存区，否则直接落盘。
    void writeDiskRaw(PageId id, const PageBytes& bytes);
    // 真正写磁盘。
    void ensureIdentity();
    // 旧格式文件升级时补上数据库身份。
    void recoverJournal(bool recovering);
    // 重做日志：把已提交但未落盘的扩展重新应用。
    // 双写缓冲（torn-page 防护，MINISQL_DOUBLEWRITE=1 时启用）：
    // 数据页在写主文件前先落入 .dwb 槽位并同步，主文件同步成功后再释放槽位；
    // 打开时用仍有效的槽位修复未完成页写。
    void recoverDoubleWrite();
    void stageDoubleWrite(const std::map<PageId, PageBytes>& pages);
    void clearDoubleWrite();
    void applyExtent(const std::map<PageId, PageBytes>& pages, std::uint64_t finalCount, bool recovering);
    // 应用一批页并调整文件长度。
    void checkpointJournal();
    // 截断日志文件。
    void writeCheckpointRecord();
    // 写 .ckpt 检查点记录。
    void loadCheckpointRecord();
    // 读 .ckpt 检查点记录。
    void notify(std::string_view stage, bool allowCrash = true);
    // 通知观察者当前阶段，并可选地按环境变量触发崩溃，用于故障注入。
    void writeHeader();
    // 写第 0 页的文件头。
    void requireActive(PageRef ref) const;
    // 校验引用指向一个有效页且代数匹配。
};
}
