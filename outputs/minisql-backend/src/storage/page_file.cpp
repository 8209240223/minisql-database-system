#include "minisql/storage/page_file.hpp"
#include "minisql/common/error.hpp"
#include <limits>
#include <algorithm>
#include <map>
#include <random>
#include <cstdlib>
#include <chrono>
#include <vector>

namespace minisql::storage {
namespace {
constexpr std::uint32_t fileMagic = 0x4644534d;
constexpr std::uint32_t freeMagic = 0x4652534d;
constexpr std::uint32_t journalMagic = 0x4a44534d;
constexpr std::uint32_t recordMagic = 0x5244534d;
constexpr std::uint32_t commitMarkerMagic = 0x434d544d;
constexpr std::uint32_t checkpointMagic = 0x4d595043;
constexpr std::uint32_t dwbMagic = 0x4257444d;   // 'MDWB'：双写缓冲头
constexpr std::uint32_t doubleWriteSlots = 32;   // 单次扩展最多暂存的页数
constexpr std::size_t defaultMaxBatchPages = 16384;
// WAL 扩展头/页记录的字段偏移与类型编号（v2 起启用记录级元数据；v1 日志仍可读）。
constexpr std::uint32_t journalVersionV1 = 1;
constexpr std::uint32_t journalVersionV2 = 2;
constexpr std::size_t hPrevLsn = 96, hRecordType = 104, hFlags = 108, hCommitLsn = 112, hUndoNextLsn = 120, hExtentLsn = 128;
constexpr std::size_t rPageId = 8, rAfterChecksum = 16, rRecordLsn = 24, rPrevRecordLsn = 32, rRecordType = 40;
constexpr std::uint32_t recordRedo = 1, recordAbort = 3;
constexpr std::size_t cLastLsn = 64, cBeginLsn = 72, cEndLsn = 80, cArchivedBytes = 88, cArchiveSegments = 96;
std::size_t maxBatchPages() {
    const auto* configured = std::getenv("MINISQL_MAX_BATCH_PAGES");
    if (!configured || !*configured) return defaultMaxBatchPages;
    char* end = nullptr;
    const auto parsed = std::strtoull(configured, &end, 10);
    if (end && *end == '\0' && parsed > 0 && parsed <= defaultMaxBatchPages)
        return static_cast<std::size_t>(parsed);
    return defaultMaxBatchPages;
}
// group commit 单组最大暂存字节；超过后立即同步，避免无界内存。
std::size_t groupCommitBytes() {
    constexpr std::size_t limit = 16ull * 1024ull * 1024ull;
    const auto* configured = std::getenv("MINISQL_GROUP_COMMIT_BYTES");
    if (!configured || !*configured) return limit;
    char* end = nullptr;
    const auto parsed = std::strtoull(configured, &end, 10);
    if (end && *end == '\0' && parsed >= kPageSize && parsed <= 1024ull * 1024ull * 1024ull)
        return static_cast<std::size_t>(parsed);
    return limit;
}
bool enabledFlag(const char* name) {
    const auto* configured = std::getenv(name);
    return configured && std::string_view(configured) == "1";
}
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
void seal(PageBytes& bytes) { writeUnsigned(bytes, 4, 4, checksum(bytes)); }
std::filesystem::path sidecar(std::filesystem::path path, const char* suffix) { path += suffix;return path; }
std::array<std::uint64_t, 2> newIdentity() {
    std::random_device random;
    std::array<std::uint64_t, 2> result{};
    do { for (auto& part : result) part = (std::uint64_t(random()) << 32) | random(); } while (result[0] == 0 && result[1] == 0);
    return result;
}
void putPage(std::ostream& output, const PageBytes& bytes) {
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!output) fail("Journal write failed");
}
}

PageFile::PageFile(const std::filesystem::path& path, CommitObserver observer)
    : path_(std::filesystem::weakly_canonical(path)), observer_(std::move(observer)) {
    if (std::filesystem::exists(path_) && std::filesystem::hard_link_count(path_) > 1) fail("Hard-linked database files are not supported");
    lock_ = std::make_unique<ExclusiveFileLock>(sidecar(path_, ".lock"));
    const auto journal = sidecar(path_, ".wal");
    const bool pendingJournal = std::filesystem::exists(journal) && std::filesystem::file_size(journal) != 0;
    if (!std::filesystem::exists(path_)) {
        if (pendingJournal) fail("Database missing while redo journal exists");
        std::ofstream create(path_, std::ios::binary);
        if (!create) fail("Cannot create page file");
        create.close();
    }
    auto size = std::filesystem::file_size(path_);
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) fail("Cannot open page file");
    loadCheckpointRecord();
    if (const char* configured = std::getenv("MINISQL_DOUBLEWRITE"); configured && std::string_view(configured) == "1")
        doubleWrite_ = true;
    if (const char* configured = std::getenv("MINISQL_GROUP_COMMIT"); configured && std::string_view(configured) == "1")
        groupCommit_ = true;    if (size == 0) {
        if (pendingJournal) fail("Database empty while redo journal exists");
        identity_ = newIdentity();writeHeader();flush();syncFile(path_);return;
    }
    // 双写缓冲先行修复未完成的页写（提供干净的基页），随后再做日志重做。
    if (doubleWrite_) { recoverDoubleWrite();size = std::filesystem::file_size(path_); }
    if (pendingJournal) { recoverJournal(true);size = std::filesystem::file_size(path_); }
    if (size < kPageSize || size % kPageSize != 0) fail("STORAGE_CORRUPTION: file length");
    const auto header = readRaw(0);
    const auto version = readUnsigned(header, 8, 4);
    if (readUnsigned(header, 0, 4) != fileMagic || (version != 1 && version != 2) ||
        readUnsigned(header, 12, 4) != kPageSize || readUnsigned(header, 4, 4) != checksum(header))
        fail("STORAGE_CORRUPTION: file header");
    count_ = readUnsigned(header, 16, 8);
    if (version == 2) {
        identity_ = {readUnsigned(header, 24, 8), readUnsigned(header, 32, 8)};
        if (identity_[0] == 0 && identity_[1] == 0) fail("STORAGE_CORRUPTION: database identity");
    }
    if (count_ != size / kPageSize) fail("STORAGE_CORRUPTION: page count");
    for (PageId id = 1; id < count_; ++id) {
        const auto bytes = readRaw(id);
        if (readUnsigned(bytes, 0, 4) == freeMagic) {
            if (readUnsigned(bytes, 4, 4) != checksum(bytes) || readUnsigned(bytes, 8, 8) != id || readUnsigned(bytes, 24, 8) == 0)
                fail("STORAGE_CORRUPTION: free page");
            free_.push_back({id, readUnsigned(bytes, 24, 8)});
        } else {
            const SlottedPage page(bytes);
            if (page.id() != id) fail("STORAGE_CORRUPTION: page identity");
            active_.emplace(id, page.generation());
            owners_.emplace(id, page.owner());
        }
    }
}
PageFile::~PageFile() {
    // 干净关闭时把 group commit 的待同步扩展落盘；异常路径不得抛出。
    try {
        syncJournalGroup();
    } catch (...) {
    }
}
PageBytes PageFile::readRaw(PageId id) {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    if (batch_) {
        const auto found = batch_->pages.find(id);
        if (found != batch_->pages.end()) return found->second;
    } else if (const auto pending = pendingPages_.find(id); pending != pendingPages_.end()) {
        // group commit 尚未应用的扩展：读必须看到已提交的新内容。
        return pending->second;
    }
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    PageBytes bytes{};
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(id * kPageSize));
    file_.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!file_ || file_.gcount() != kPageSize) { ++ioStats_.errors; failed_ = true; fail("Page short read or I/O failure"); }
    ++ioStats_.reads;
    return bytes;
}
void PageFile::writeRaw(PageId id, const PageBytes& bytes) {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    if (batch_) {
        if (!batch_->pages.contains(id) && batch_->pages.size() >= maxBatchPages())
            throw MiniSqlError(ErrorCode::Transaction, "Write batch page limit exceeded; rollback required");
        batch_->touched = true;
        batch_->pages.insert_or_assign(id, bytes);return;
    }
    writeDiskRaw(id, bytes);
}
void PageFile::writeDiskRaw(PageId id, const PageBytes& bytes) {
    requireHealthy();
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(id * kPageSize));
    file_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file_) { ++ioStats_.errors; failed_ = true; fail("Page write I/O failure"); }
    ++ioStats_.writes;
}
void PageFile::writeHeader() {
    PageBytes header{};
    writeUnsigned(header, 0, 4, fileMagic);
    writeUnsigned(header, 8, 4, identity_[0] == 0 && identity_[1] == 0 ? 1 : 2);
    writeUnsigned(header, 12, 4, kPageSize);
    writeUnsigned(header, 16, 8, count_);
    writeUnsigned(header, 24, 8, identity_[0]);
    writeUnsigned(header, 32, 8, identity_[1]);
    seal(header);
    writeRaw(0, header);
}
void PageFile::requireActive(PageRef ref) const {
    const auto found = active_.find(ref.id);
    if (found == active_.end() || found->second != ref.generation) fail("STALE_PAGE_ID");
}
PageRef PageFile::allocate(std::uint64_t owner) {
    PageRef ref{count_, 1};
    const bool reused = !free_.empty();
    if (reused) {
        ref = free_.back();
        if (ref.generation == std::numeric_limits<std::uint64_t>::max()) fail("Page generation exhausted");
        ++ref.generation;
    }
    writeRaw(ref.id, SlottedPage(ref.id, owner, ref.generation).serialize());
    if (!reused) { ++count_; writeHeader(); }
    flush();
    if (reused) free_.pop_back();
    active_.emplace(ref.id, ref.generation);
    owners_.emplace(ref.id, owner);
    return ref;
}
SlottedPage PageFile::read(PageRef ref) {
    requireActive(ref);
    SlottedPage page(readRaw(ref.id));
    if (page.id() != ref.id || page.generation() != ref.generation || page.owner() != owners_.at(ref.id)) fail("STORAGE_CORRUPTION: page ownership");
    return page;
}
void PageFile::write(const SlottedPage& page) {
    requireActive({page.id(), page.generation()});
    if (page.owner() != owners_.at(page.id())) fail("Page owner mismatch");
    writeRaw(page.id(), page.serialize());
}
void PageFile::release(PageRef ref) {
    requireActive(ref);
    PageBytes bytes{};
    writeUnsigned(bytes, 0, 4, freeMagic);
    writeUnsigned(bytes, 8, 8, ref.id);
    writeUnsigned(bytes, 24, 8, ref.generation);
    seal(bytes);
    writeRaw(ref.id, bytes);
    flush();
    active_.erase(ref.id);
    owners_.erase(ref.id);
    free_.push_back(ref);
}
void PageFile::flush() {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    file_.flush();
    if (!file_) { failed_ = true; fail("Page flush I/O failure"); }
}
void PageFile::beginWriteBatch() {
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Nested write batches are not supported");
    flush();
    ensureIdentity();
    syncFile(path_);
    auto snapshot = std::make_unique<WriteBatch>();
    snapshot->count = count_;
    snapshot->active = active_;
    snapshot->owners = owners_;
    snapshot->free = free_;
    snapshot->txId = ++txSequence_;
    snapshot->startLsn = walBytes();
    snapshot->prevLsn = lastExtentLsn_;
    snapshot->extentLsn = nextLsn_;
    batch_ = std::move(snapshot);
}
void PageFile::rollbackWriteBatch() {
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
    // 丢弃本批次已追加但未提交（无提交标记）的预备扩展，避免其阻塞其后已提交扩展的恢复。
    const auto journal = sidecar(path_, ".wal");
    const auto startOffset = batch_->startLsn;
    if (walBytes() > startOffset) std::filesystem::resize_file(journal, startOffset);
    const bool touched = batch_->touched;
    const auto txId = batch_->txId, prevLsn = batch_->prevLsn, extentLsn = batch_->extentLsn, baseCount = batch_->count;
    count_ = batch_->count;
    active_ = std::move(batch_->active);
    owners_ = std::move(batch_->owners);
    free_ = std::move(batch_->free);
    batch_.reset();
    if (!touched) return;
    // 记录级撤销：把回滚本身写成一条 Abort 记录（携带 prevLsn/undoNextLsn 链），
    // 使未提交修改的撤销在日志中可见、可分析，而不是无声消失。
    try {
        std::ofstream output(journal, std::ios::binary | std::ios::app);
        if (!output) fail("Cannot append abort record");
        PageBytes header{};
        writeUnsigned(header, 0, 4, journalMagic);writeUnsigned(header, 8, 4, journalVersionV2);writeUnsigned(header, 12, 4, kPageSize);
        writeUnsigned(header, 16, 8, 0);writeUnsigned(header, 24, 8, count_);
        writeUnsigned(header, 32, 8, identity_[0]);writeUnsigned(header, 40, 8, identity_[1]);
        writeUnsigned(header, 48, 8, baseCount);writeUnsigned(header, 56, 8, committedSequence_);writeUnsigned(header, 64, 8, txId);
        writeUnsigned(header, 72, 8, startOffset);writeUnsigned(header, 80, 8, startOffset + 2 * kPageSize);
        writeUnsigned(header, 88, 8, checkpointRecord_.present ? checkpointRecord_.walCutoffBytes : 0);
        writeUnsigned(header, hPrevLsn, 8, prevLsn);
        writeUnsigned(header, hRecordType, 4, recordAbort);
        writeUnsigned(header, hFlags, 4, 0);
        writeUnsigned(header, hCommitLsn, 8, 0);
        writeUnsigned(header, hUndoNextLsn, 8, prevLsn);
        writeUnsigned(header, hExtentLsn, 8, extentLsn);
        seal(header);
        putPage(output, header);
        PageBytes terminator{};
        writeUnsigned(terminator, 0, 4, commitMarkerMagic);writeUnsigned(terminator, 8, 8, 0);seal(terminator);
        putPage(output, terminator);
        output.flush();if (!output) fail("Abort record write failed");syncFile(journal);
        output.close();if (!output) fail("Cannot close abort record");
        ++abortedExtents_;
        lastExtentLsn_ = extentLsn;
        nextLsn_ = std::max(nextLsn_, extentLsn + 1);
        transactionLsn_.insert_or_assign(txId, extentLsn);
    } catch (...) { failed_ = true;throw; }
}
void PageFile::requireHealthy() const {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
}
void PageFile::ensureIdentity() {
    if (identity_[0] != 0 || identity_[1] != 0) return;
    const auto identity = newIdentity();
    const auto temporary = sidecar(path_, ".upgrade.tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    auto header = readRaw(0);
    writeUnsigned(header, 8, 4, 2);writeUnsigned(header, 24, 8, identity[0]);writeUnsigned(header, 32, 8, identity[1]);seal(header);
    putPage(output, header);
    for (PageId id = 1; id < count_; ++id) putPage(output, readRaw(id));
    output.close();if (!output) fail("Legacy header upgrade write failed");
    syncFile(temporary);
    file_.close();
    try { publishFile(temporary, path_); }
    catch (...) { failed_ = true;throw; }
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) { failed_ = true;fail("Cannot reopen upgraded database"); }
    identity_ = identity;
}
void PageFile::commitWriteBatch() {
    requireHealthy();
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    if (batch_->pages.empty()) { lastCommitWalBytes_ = 0; batch_.reset();return; }
    const auto journal = sidecar(path_, ".wal");
    writeHeader();
    const std::map<PageId, PageBytes> ordered(batch_->pages.begin(), batch_->pages.end());
    const auto seq = committedSequence_ + 1;
    const auto startLsn = walBytes();
    const auto extentLsn = batch_->extentLsn;
    const auto prevLsn = batch_->prevLsn;
    // 扩展字节 = 头(1 页) + 每条(记录 1 页 + 数据 1 页) + 提交标记(1 页)。
    // commitLsn 指向提交标记页；endLsn 为本次扩展末尾（不含）偏移。
    const auto commitLsn = startLsn + (2 * ordered.size() + 1) * kPageSize;
    const auto endLsn = startLsn + 2 * (ordered.size() + 1) * kPageSize;
    bool publishedMarker = false;
    try {
        std::ofstream output(journal, std::ios::binary | std::ios::app);
        PageBytes header{};
        writeUnsigned(header, 0, 4, journalMagic);writeUnsigned(header, 8, 4, journalVersionV2);writeUnsigned(header, 12, 4, kPageSize);
        writeUnsigned(header, 16, 8, ordered.size());writeUnsigned(header, 24, 8, count_);
        writeUnsigned(header, 32, 8, identity_[0]);writeUnsigned(header, 40, 8, identity_[1]);
        writeUnsigned(header, 48, 8, batch_->count);writeUnsigned(header, 56, 8, seq);writeUnsigned(header, 64, 8, batch_->txId);
        // 扩展字节 = 头(1 页) + 每条(记录 1 页 + 数据 1 页) + 提交标记(1 页)；endLsn 为本次扩展末尾偏移。
        writeUnsigned(header, 72, 8, startLsn);writeUnsigned(header, 80, 8, endLsn);
        writeUnsigned(header, 88, 8, checkpointRecord_.present ? checkpointRecord_.walCutoffBytes : 0);
        // 记录级元数据：全局 prevLsn 链、扩展类型/逻辑 LSN、提交标记 LSN、事务内撤销链。
        writeUnsigned(header, hPrevLsn, 8, prevLsn);
        writeUnsigned(header, hRecordType, 4, recordRedo);
        writeUnsigned(header, hFlags, 4, 0);
        writeUnsigned(header, hCommitLsn, 8, commitLsn);
        writeUnsigned(header, hUndoNextLsn, 8, prevLsn);
        writeUnsigned(header, hExtentLsn, 8, extentLsn);
        seal(header);
        putPage(output, header);
        std::uint64_t recordLsn = startLsn + kPageSize;
        for (const auto& [id, bytes] : ordered) {
            const auto previous = pageLsn_.contains(id) ? pageLsn_.at(id) : 0;
            PageBytes record{};
            writeUnsigned(record, 0, 4, recordMagic);writeUnsigned(record, rPageId, 8, id);
            writeUnsigned(record, rAfterChecksum, 4, checksum(bytes));
            writeUnsigned(record, rRecordLsn, 8, recordLsn);
            writeUnsigned(record, rPrevRecordLsn, 8, previous);
            writeUnsigned(record, rRecordType, 4, recordRedo);
            seal(record);
            putPage(output, record);putPage(output, bytes);
            pageLsn_.insert_or_assign(id, recordLsn);
            ++recordedPages_;
            recordLsn += 2 * kPageSize;
        }
        output.flush();if (!output) fail("Journal prepare failed");
        if (!groupCommit_) syncFile(journal);
        notify("prepared");
        header[0]; // 已写盘（header+data），随后追加提交标记形成"已提交"。
        PageBytes marker{};
        writeUnsigned(marker, 0, 4, commitMarkerMagic);writeUnsigned(marker, 8, 8, seq);seal(marker);
        putPage(output, marker);
        output.flush();if (!output) fail("Journal commit failed");
        if (!groupCommit_) syncFile(journal);
        publishedMarker = true;
        notify("published");
        output.close();if (!output) fail("Journal close failed");
        lastCommitWalBytes_ = walBytes() - startLsn;
        batch_->published = true;
    } catch (...) {
        if (publishedMarker) { batch_->published = true;failed_ = true; }
        lastCommitWalBytes_ = walBytes() - startLsn;
        throw;
    }
    // 数据即刻落盘保持 DB 现行（日志仅作 REDO，恢复幂等），此时提交点已越过，可安全推进提交序号。
    ++committedSequence_;
    ++committedExtents_;
    lastExtentLsn_ = extentLsn;
    nextLsn_ = std::max(nextLsn_, extentLsn + 1);
    transactionLsn_.insert_or_assign(batch_->txId, extentLsn);
    if (groupCommit_) {
        // group commit：日志已追加但未 fsync，数据页也先不应用；
        // 真正的稳定点在同一次 group 同步：先 fsync 日志，再按序应用全部扩展。
        PendingExtent deferred;
        deferred.pages = ordered;
        deferred.finalCount = count_;
        pendingBytes_ += ordered.size() * kPageSize;
        for (const auto& [id, bytes] : ordered) pendingPages_.insert_or_assign(id, bytes);
        pendingExtents_.push_back(std::move(deferred));
        batch_.reset();
        notify("group-pending");
        if (pendingBytes_ >= groupCommitBytes()) syncJournalGroup();
        return;
    }
    applyExtent(ordered, count_, false);
    batch_.reset();
    notify("checkpointed");
}
void PageFile::syncJournalGroup() {
    if (pendingExtents_.empty()) return;
    const auto journal = sidecar(path_, ".wal");
    // 一次性把整组日志刷到稳定存储，随后才把数据页写入主文件；
    // 若在此期间崩溃，标记未落盘，恢复会丢弃整组（不会出现无标记数据）。
    if (std::filesystem::exists(journal)) syncFile(journal);
    auto pending = std::move(pendingExtents_);
    pendingExtents_.clear();
    pendingPages_.clear();
    pendingBytes_ = 0;
    for (auto& extent : pending) applyExtent(extent.pages, extent.finalCount, false);
}
PageFileSavepoint PageFile::savepoint() const {
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    return {count_, active_, owners_, free_, batch_->pages, batch_->published};
}
void PageFile::restoreSavepoint(const PageFileSavepoint& snapshot) {
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
    count_ = snapshot.count;
    active_ = snapshot.active;
    owners_ = snapshot.owners;
    free_ = snapshot.free;
    batch_->pages = snapshot.pages;
    batch_->published = snapshot.published;
}
void PageFile::checkpoint(const CheckpointOptions& options) {
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Cannot checkpoint during a write batch");
    // 检查点前先把 group commit 的待同步扩展落盘，否则会截断尚未应用的日志。
    syncJournalGroup();
    const bool fuzzy = options.fuzzy || enabledFlag("MINISQL_FUZZY_CHECKPOINT");
    const bool archive = options.archive || enabledFlag("MINISQL_ARCHIVE_WAL");
    const auto watermark = nextLsn_ > 0 ? nextLsn_ - 1 : 0;
    checkpointRecord_.present = true;
    checkpointRecord_.dirtyWatermark = count_;
    dirtyWatermark_ = count_;
    checkpointRecord_.catalogVersion = options.catalogVersion;
    checkpointRecord_.indexVersion = options.indexVersion;
    checkpointRecord_.committedSequence = committedSequence_;
    checkpointRecord_.walLsn = watermark;
    if (fuzzy) {
        // 模糊检查点：记录起始/结束 LSN 与截止位置，不截断日志；
        // 已提交数据页在提交时已落盘，恢复从截止位置重做其后的扩展。
        checkpointRecord_.checkpointBeginLsn = watermark;
        checkpointRecord_.checkpointEndLsn = watermark;
        checkpointRecord_.walCutoffBytes = walBytes();
        writeCheckpointRecord();
        return;
    }
    // 非模糊检查点：回收上一次截止位置之前的日志（可选归档），恢复起点归零。
    checkpointRecord_.checkpointBeginLsn = 0;
    checkpointRecord_.checkpointEndLsn = 0;
    const auto previousCutoff = checkpointRecord_.walCutoffBytes;
    checkpointRecord_.walCutoffBytes = 0;
    recycleJournal(archive, previousCutoff);
    writeCheckpointRecord();
}
// 安全回收：只删除已经提交且已落盘、且不被当前检查点后的恢复所需的日志前缀。
// 可选先把被回收的前缀归档到 .wal.archive.N，并追加一行归档清单。
void PageFile::recycleJournal(bool archive, std::uint64_t cutoff) {
    const auto journal = sidecar(path_, ".wal");
    std::error_code error;
    if (!std::filesystem::exists(journal, error)) return;
    const auto size = std::filesystem::file_size(journal, error);
    if (error) fail("Cannot inspect redo journal for recycling");
    if (size == 0) return;
    // cutoff==0 表示没有较早的恢复起点，整段日志都可回收；否则只回收检查点覆盖的前缀。
    const auto recycleBytes = cutoff == 0 ? size : std::min(cutoff, size);
    if (recycleBytes == 0) return;
    if (archive) {
        const auto segment = checkpointRecord_.archiveSegments + 1;
        auto target = sidecar(path_, ".wal.archive.");
        target += std::to_string(segment);
        std::ifstream input(journal, std::ios::binary);
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        std::vector<char> prefix(static_cast<std::size_t>(recycleBytes));
        input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        output.write(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        output.flush();
        if (!input || !output) fail("Cannot archive redo journal prefix");
        output.close();
        syncFile(target);
        checkpointRecord_.archiveSegments = segment;
        checkpointRecord_.archivedBytes += recycleBytes;
        std::ofstream manifest(sidecar(path_, ".wal.archive.log"), std::ios::app);
        if (manifest) manifest << "segment=" << segment << " bytes=" << recycleBytes << " lsn=" << checkpointRecord_.walLsn << '\n';
        manifest.flush();
    }
    if (recycleBytes >= size) { checkpointJournal(); return; }
    std::vector<char> tail(static_cast<std::size_t>(size - recycleBytes));
    {
        std::ifstream input(journal, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(recycleBytes));
        input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
        if (!input) fail("Cannot read redo journal tail");
    }
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    output.write(tail.data(), static_cast<std::streamsize>(tail.size()));
    output.flush();
    if (!output) fail("Cannot rewrite redo journal");
    output.close();
    syncFile(journal);
}
void PageFile::checkpointJournal() {
    const auto journal = sidecar(path_, ".wal");
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    if (!output) fail("Cannot truncate redo journal");
    output.close();
    if (!output) fail("Cannot close redo journal");
    syncFile(journal);
}

void PageFile::copyTo(const std::filesystem::path& destination) {
    requireHealthy();
    // 快照必须包含尚未应用的 group commit 扩展，先同步整组。
    syncJournalGroup();
    if (destination.empty()) fail("Snapshot destination is empty");
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::exists(destination)) fail("Snapshot destination already exists");
    std::filesystem::copy_file(path_, destination);
    if (const auto journal = sidecar(path_, ".wal"); std::filesystem::exists(journal)) {
        std::filesystem::copy_file(journal, sidecar(destination, ".wal"));
    }
    if (const auto ckpt = sidecar(path_, ".ckpt"); std::filesystem::exists(ckpt)) {
        std::filesystem::copy_file(ckpt, sidecar(destination, ".ckpt"));
    }
}
void PageFile::notify(std::string_view stage, bool allowCrash) {
    if (observer_) observer_(stage);
    if (!allowCrash) return;
    const auto configured = std::getenv("MINISQL_CRASH_AT");
    if (configured && std::string_view(configured) == stage) std::_Exit(77);
}
std::uint64_t PageFile::walBytes() const {
    std::error_code error;
    const auto journal = sidecar(path_, ".wal");
    if (!std::filesystem::exists(journal, error)) {
        if (error) fail("Cannot inspect redo journal");
        return 0;
    }
    const auto size = std::filesystem::file_size(journal, error);
    if (error) fail("Cannot inspect redo journal");
    return size;
}
WalStatistics PageFile::walStatistics() const {
    WalStatistics result;
    result.walBytes = walBytes();
    result.nextLsn = nextLsn_;
    result.lastExtentLsn = lastExtentLsn_;
    result.lastCheckpointLsn = checkpointRecord_.walLsn;
    result.committedSequence = committedSequence_;
    result.dirtyWatermark = dirtyWatermark_;
    result.committedExtents = committedExtents_;
    result.abortedExtents = abortedExtents_;
    result.recordedPages = recordedPages_;
    result.trackedPages = pageLsn_.size();
    result.archivedBytes = checkpointRecord_.archivedBytes;
    result.archiveSegments = checkpointRecord_.archiveSegments;
    result.doubleWrite = doubleWrite_;
    result.groupCommit = groupCommit_;
    result.pendingCommitBytes = pendingBytes_;
    result.fuzzyCheckpoint = checkpointRecord_.checkpointBeginLsn != 0;
    return result;
}
void PageFile::recoverJournal(bool recovering) {
    const auto journal = sidecar(path_, ".wal");
    if (!std::filesystem::exists(journal)) return;
    std::ifstream input(journal, std::ios::binary);
    if (!input) fail("Cannot open redo journal");
    const auto size = std::filesystem::file_size(journal);
    if (size == 0) return;
    // 非零截止位置重做：若 .ckpt 记录了本次日志的截止字节且日志保留该前缀，则跳过已落盘前缀，仅重做其后的已提交扩展。
    std::uint64_t offset = checkpointRecord_.present ? checkpointRecord_.walCutoffBytes : 0;
    if (offset > size || offset % kPageSize != 0) offset = 0;
    if (offset > 0) input.seekg(static_cast<std::streamoff>(offset));
    auto readPage = [&](PageBytes& out) -> bool {
        out = PageBytes{};
        input.clear();
        input.read(reinterpret_cast<char*>(out.data()), kPageSize);
        const auto got = input.gcount();
        if (got == 0) return false;                                        // 页边界处的干净 EOF（未提交尾或无后续）
        if (got != kPageSize) { ++ioStats_.errors;failed_ = true;fail("STORAGE_CORRUPTION: redo truncated"); }
        return true;
    };
    const auto validatePage = [&](const PageBytes& bytes, PageId id, std::uint64_t finalCount, const std::array<std::uint64_t, 2>& identity) {
        if (id == 0) {
            if (readUnsigned(bytes, 0, 4) != fileMagic || readUnsigned(bytes, 8, 4) != 2 || readUnsigned(bytes, 12, 4) != kPageSize ||
                readUnsigned(bytes, 16, 8) != finalCount || readUnsigned(bytes, 24, 8) != identity[0] || readUnsigned(bytes, 32, 8) != identity[1])
                fail("STORAGE_CORRUPTION: redo file header");
        } else if (readUnsigned(bytes, 0, 4) == freeMagic) {
            if (readUnsigned(bytes, 8, 8) != id || readUnsigned(bytes, 24, 8) == 0) fail("STORAGE_CORRUPTION: redo free page");
        } else if (SlottedPage(bytes).id() != id) fail("STORAGE_CORRUPTION: redo page identity");
    };
    PageBytes lookahead;
    bool has = readPage(lookahead);
    std::uint64_t lastAppliedSeq = 0;   // 本次重做最后成功应用扩展的提交序号（LSN），用于恢复后保持序号连续
    while (has) {
        const auto header = lookahead;
        if (readUnsigned(header, 0, 4) != journalMagic) break;   // 遍历到异常/标记残留处停止
        const auto version = readUnsigned(header, 8, 4);
        if ((version != journalVersionV1 && version != journalVersionV2) || readUnsigned(header, 12, 4) != kPageSize ||
            readUnsigned(header, 4, 4) != checksum(header)) fail("STORAGE_CORRUPTION: redo header");
        const auto records = readUnsigned(header, 16, 8), finalCount = readUnsigned(header, 24, 8), baseCount = readUnsigned(header, 48, 8);
        const auto recordType = version >= journalVersionV2 ? readUnsigned(header, hRecordType, 4) : recordRedo;
        // v2 起扩展头带逻辑 LSN；v1 用提交序号退化为 LSN，保持旧日志可分析。
        const auto extentLsn = version >= journalVersionV2 ? readUnsigned(header, hExtentLsn, 8) : readUnsigned(header, 56, 8);
        // 撤销（Abort）扩展：没有页记录，仅标记一个事务已回滚；跳过并继续其后扩展。
        if (recordType == recordAbort) {
            if (records != 0 || finalCount == 0) fail("STORAGE_CORRUPTION: redo abort record");
            PageBytes terminator{};
            if (!readPage(terminator)) break;
            if (readUnsigned(terminator, 0, 4) != commitMarkerMagic) break;
            lastExtentLsn_ = std::max(lastExtentLsn_, extentLsn);
            nextLsn_ = std::max(nextLsn_, extentLsn + 1);
            has = readPage(lookahead);
            continue;
        }
        if (records == 0 || records > maxBatchPages() || baseCount == 0 || finalCount < baseCount || finalCount - baseCount > records ||
            finalCount > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("STORAGE_CORRUPTION: redo header fields");
        const std::array<std::uint64_t, 2> identity{readUnsigned(header, 32, 8), readUnsigned(header, 40, 8)};
        const auto original = readRaw(0);
        if ((identity[0] == 0 && identity[1] == 0) || identity[0] != readUnsigned(original, 24, 8) || identity[1] != readUnsigned(original, 32, 8))
            fail("STORAGE_CORRUPTION: redo database identity mismatch");
        std::map<PageId, PageBytes> pages;
        std::map<PageId, std::uint64_t> recordLsns;   // 仅在提交确认后并入页级 LSN 链
        for (std::uint64_t i = 0; i < records; ++i) {
            PageBytes record{}, bytes{};
            if (!readPage(record) || !readPage(bytes)) fail("STORAGE_CORRUPTION: redo truncated");
            const auto id = readUnsigned(record, rPageId, 8);
            if (readUnsigned(record, 0, 4) != recordMagic || readUnsigned(record, 4, 4) != checksum(record) || id >= finalCount ||
                readUnsigned(record, rAfterChecksum, 4) != checksum(bytes) || !pages.emplace(id, bytes).second) fail("STORAGE_CORRUPTION: redo record");
            validatePage(bytes, id, finalCount, identity);
            if (version >= journalVersionV2) recordLsns.insert_or_assign(id, readUnsigned(record, rRecordLsn, 8));
        }
        if (!pages.contains(0)) fail("STORAGE_CORRUPTION: redo missing file header");
        for (auto id = baseCount; id < finalCount; ++id) if (!pages.contains(id)) fail("STORAGE_CORRUPTION: redo missing allocated page");
        PageBytes marker{};
        const bool hasMarker = readPage(marker);
        if (!hasMarker) break;   // 净 EOF：预备但未提交（无提交标记）的尾部，丢弃
        if (readUnsigned(marker, 0, 4) != commitMarkerMagic) break;   // 标记缺失/不匹配：停止（其后无更优提交）
        const auto markerSeq = readUnsigned(marker, 8, 8);
        lastExtentLsn_ = std::max(lastExtentLsn_, extentLsn);
        nextLsn_ = std::max(nextLsn_, extentLsn + 1);
        if (markerSeq == 0) { has = readPage(lookahead);continue; }   // 未提交扩展：丢弃并继续其后扩展
        if (markerSeq != readUnsigned(header, 56, 8)) break;
        lastAppliedSeq = markerSeq;
        for (const auto& [id, lsn] : recordLsns) pageLsn_.insert_or_assign(id, lsn);
        ++committedExtents_;
        recordedPages_ += records;
        applyExtent(pages, finalCount, recovering);   // 已提交：落盘（幂等）
        has = readPage(lookahead);
    }
    input.close();
    // 保持 LSN 语义连续：重做所达的最后提交序号在无 .ckpt（或早于日志）时也须保留，供后续提交序号递增。
    if (lastAppliedSeq > committedSequence_) committedSequence_ = lastAppliedSeq;
    // 逻辑 LSN 必须跨日志截断保持单调：把已消耗到的水位随检查点记录持久化。
    checkpointRecord_.walLsn = std::max(checkpointRecord_.walLsn, nextLsn_ > 0 ? nextLsn_ - 1 : 0);
    // 持久化恢复所达提交序号：期刊已清空（截止归零），若不落盘，干净重启会回退到旧 .ckpt 序号并复用 LSN。
    if (lastAppliedSeq > 0 || nextLsn_ > 1) {
        checkpointRecord_.present = true;
        checkpointRecord_.walCutoffBytes = 0;
        checkpointRecord_.committedSequence = committedSequence_;
        writeCheckpointRecord();
    }
    // 日志回收：已提交数据均已落盘且无未结束资源，整段日志可截止。
    checkpointJournal();
    notify("checkpointed", false);
}
void PageFile::applyExtent(const std::map<PageId, PageBytes>& pages, std::uint64_t finalCount, bool recovering) {
    // 双写缓冲：把本扩展的页先落入 .dwb 并同步，主文件页写未完成时仍可修复。
    if (!recovering) {
        stageDoubleWrite(pages);
        notify("doublewrite-staged");
    }
    for (const auto& [id, bytes] : pages) if (id != 0) {
        writeDiskRaw(id, bytes);
        flush();
        notify(recovering ? "recovery-page" : "applied-page");
    }
    writeDiskRaw(0, pages.at(0));flush();
    std::filesystem::resize_file(path_, finalCount * kPageSize);
    syncFile(path_);
    notify(recovering ? "recovery-synced" : "data-synced");
    if (!recovering) clearDoubleWrite();
}
void PageFile::recoverDoubleWrite() {
    const auto dwb = sidecar(path_, ".dwb");
    if (!std::filesystem::exists(dwb)) return;
    if (std::filesystem::file_size(dwb) < static_cast<std::uint64_t>(doubleWriteSlots + 1) * kPageSize)
        fail("STORAGE_CORRUPTION: doublewrite file length");
    std::fstream file(dwb, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) fail("Cannot open doublewrite file");
    PageBytes header{};
    file.read(reinterpret_cast<char*>(header.data()), kPageSize);
    if (!file || readUnsigned(header, 0, 4) != dwbMagic || readUnsigned(header, 4, 4) != checksum(header) ||
        readUnsigned(header, 8, 4) != 1 || readUnsigned(header, 12, 4) != doubleWriteSlots)
        fail("STORAGE_CORRUPTION: doublewrite header");
    std::uint64_t restored = 0, highest = 0;
    for (std::uint32_t slot = 0; slot < doubleWriteSlots; ++slot) {
        if (readUnsigned(header, 32 + slot, 1) == 0) continue;
        const auto id = readUnsigned(header, 64 + slot * 8, 8);
        PageBytes bytes{};
        file.clear();
        file.seekg(static_cast<std::streamoff>((1 + slot) * kPageSize));
        file.read(reinterpret_cast<char*>(bytes.data()), kPageSize);
        if (!file || readUnsigned(bytes, 4, 4) != checksum(bytes)) fail("STORAGE_CORRUPTION: doublewrite slot");
        writeDiskRaw(id, bytes);
        flush();
        ++restored;
        highest = std::max(highest, id + 1);
    }
    if (restored > 0) {
        const auto required = highest * kPageSize;
        if (std::filesystem::file_size(path_) < required) std::filesystem::resize_file(path_, required);
        syncFile(path_);
    }
    file.close();
    clearDoubleWrite();
}
void PageFile::stageDoubleWrite(const std::map<PageId, PageBytes>& pages) {
    if (!doubleWrite_ || pages.empty()) return;
    const auto dwb = sidecar(path_, ".dwb");
    if (!std::filesystem::exists(dwb)) {
        std::ofstream create(dwb, std::ios::binary | std::ios::trunc);
        if (!create) fail("Cannot create doublewrite file");
        PageBytes header{};
        writeUnsigned(header, 0, 4, dwbMagic);writeUnsigned(header, 8, 4, 1);writeUnsigned(header, 12, 4, doubleWriteSlots);
        writeUnsigned(header, 4, 4, checksum(header));
        create.write(reinterpret_cast<const char*>(header.data()), kPageSize);
        const PageBytes zero{};
        for (std::uint32_t slot = 0; slot < doubleWriteSlots; ++slot) create.write(reinterpret_cast<const char*>(zero.data()), kPageSize);
        if (!create) fail("Cannot initialise doublewrite file");
        create.close();
        syncFile(dwb);
    }
    std::fstream file(dwb, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) fail("Cannot open doublewrite file");
    PageBytes header{};
    file.read(reinterpret_cast<char*>(header.data()), kPageSize);
    if (!file || readUnsigned(header, 0, 4) != dwbMagic) fail("STORAGE_CORRUPTION: doublewrite header");
    std::uint32_t slot = 0;
    for (const auto& [id, bytes] : pages) {
        if (slot >= doubleWriteSlots) break;   // 超出槽位数量时退化为直接写主文件
        file.clear();
        file.seekp(static_cast<std::streamoff>((1 + slot) * kPageSize));
        file.write(reinterpret_cast<const char*>(bytes.data()), kPageSize);
        writeUnsigned(header, 32 + slot, 1, 1);
        writeUnsigned(header, 64 + slot * 8, 8, id);
        ++slot;
    }
    writeUnsigned(header, 4, 4, checksum(header));
    file.clear();
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(header.data()), kPageSize);
    file.flush();
    if (!file) fail("Doublewrite staging failed");
    file.close();
    syncFile(dwb);
}
void PageFile::clearDoubleWrite() {
    if (!doubleWrite_) return;
    const auto dwb = sidecar(path_, ".dwb");
    if (!std::filesystem::exists(dwb)) return;
    std::fstream file(dwb, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return;
    PageBytes header{};
    file.read(reinterpret_cast<char*>(header.data()), kPageSize);
    if (!file || readUnsigned(header, 0, 4) != dwbMagic) return;
    std::fill(header.begin() + 32, header.begin() + 32 + doubleWriteSlots, 0);
    writeUnsigned(header, 4, 4, checksum(header));
    file.clear();
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(header.data()), kPageSize);
    file.flush();
    file.close();
    syncFile(dwb);
}
std::vector<PageRef> PageFile::pagesFor(std::uint64_t owner) const {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    std::vector<PageRef> result;
    for (const auto& [id, value] : owners_) if (value == owner) result.push_back({id, active_.at(id)});
    std::sort(result.begin(), result.end(), [](const PageRef& a, const PageRef& b) { return a.id < b.id; });
    return result;
}
void PageFile::writeCheckpointRecord() {
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    checkpointRecord_.timestampMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    PageBytes bytes{};
    writeUnsigned(bytes, 0, 4, checkpointMagic);
    writeUnsigned(bytes, 8, 4, 1);
    writeUnsigned(bytes, 16, 8, checkpointRecord_.walCutoffBytes);
    writeUnsigned(bytes, 24, 8, checkpointRecord_.dirtyWatermark);
    writeUnsigned(bytes, 32, 8, checkpointRecord_.catalogVersion);
    writeUnsigned(bytes, 40, 8, checkpointRecord_.indexVersion);
    writeUnsigned(bytes, 48, 8, checkpointRecord_.committedSequence);
    writeUnsigned(bytes, 56, 8, checkpointRecord_.timestampMs);
    writeUnsigned(bytes, cLastLsn, 8, checkpointRecord_.walLsn);
    writeUnsigned(bytes, cBeginLsn, 8, checkpointRecord_.checkpointBeginLsn);
    writeUnsigned(bytes, cEndLsn, 8, checkpointRecord_.checkpointEndLsn);
    writeUnsigned(bytes, cArchivedBytes, 8, checkpointRecord_.archivedBytes);
    writeUnsigned(bytes, cArchiveSegments, 8, checkpointRecord_.archiveSegments);
    writeUnsigned(bytes, 4, 4, checksum(bytes));
    const auto ckpt = sidecar(path_, ".ckpt");
    std::ofstream output(ckpt, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!output) fail("Checkpoint record write failed");
    output.close();
    if (!output) fail("Cannot close checkpoint record");
    syncFile(ckpt);
}
void PageFile::loadCheckpointRecord() {
    const auto ckpt = sidecar(path_, ".ckpt");
    if (!std::filesystem::exists(ckpt)) return;
    if (std::filesystem::file_size(ckpt) != kPageSize) fail("STORAGE_CORRUPTION: checkpoint record");
    std::ifstream input(ckpt, std::ios::binary);
    PageBytes bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input || input.gcount() != kPageSize || readUnsigned(bytes, 0, 4) != checkpointMagic ||
        readUnsigned(bytes, 4, 4) != checksum(bytes)) fail("STORAGE_CORRUPTION: checkpoint record");
    checkpointRecord_.present = true;
    checkpointRecord_.walCutoffBytes = readUnsigned(bytes, 16, 8);
    checkpointRecord_.dirtyWatermark = readUnsigned(bytes, 24, 8);
    checkpointRecord_.catalogVersion = readUnsigned(bytes, 32, 8);
    checkpointRecord_.indexVersion = readUnsigned(bytes, 40, 8);
    checkpointRecord_.committedSequence = readUnsigned(bytes, 48, 8);
    checkpointRecord_.timestampMs = readUnsigned(bytes, 56, 8);
    checkpointRecord_.walLsn = readUnsigned(bytes, cLastLsn, 8);
    checkpointRecord_.checkpointBeginLsn = readUnsigned(bytes, cBeginLsn, 8);
    checkpointRecord_.checkpointEndLsn = readUnsigned(bytes, cEndLsn, 8);
    checkpointRecord_.archivedBytes = readUnsigned(bytes, cArchivedBytes, 8);
    checkpointRecord_.archiveSegments = readUnsigned(bytes, cArchiveSegments, 8);
    dirtyWatermark_ = checkpointRecord_.dirtyWatermark;
    committedSequence_ = checkpointRecord_.committedSequence;
    nextLsn_ = checkpointRecord_.walLsn > 0 ? checkpointRecord_.walLsn + 1 : 1;
    // 日志截断后 prevLsn 链尾也要恢复，否则截断后的第一条扩展会断开链。
    lastExtentLsn_ = checkpointRecord_.walLsn;
}
}
