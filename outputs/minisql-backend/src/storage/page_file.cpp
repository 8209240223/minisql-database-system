#include "minisql/storage/page_file.hpp"
#include "minisql/common/error.hpp"
#include <limits>
#include <algorithm>
#include <map>
#include <random>
#include <cstdlib>
#include <chrono>

namespace minisql::storage {
namespace {
constexpr std::uint32_t fileMagic = 0x4644534d;
constexpr std::uint32_t freeMagic = 0x4652534d;
constexpr std::uint32_t journalMagic = 0x4a44534d;
constexpr std::uint32_t recordMagic = 0x5244534d;
constexpr std::uint32_t commitMarkerMagic = 0x434d544d;
constexpr std::uint32_t checkpointMagic = 0x4d595043;
constexpr std::size_t maxBatchPages = 16384;
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
    if (size == 0) {
        if (pendingJournal) fail("Database empty while redo journal exists");
        identity_ = newIdentity();writeHeader();flush();syncFile(path_);return;
    }
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
PageBytes PageFile::readRaw(PageId id) {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    if (batch_) {
        const auto found = batch_->pages.find(id);
        if (found != batch_->pages.end()) return found->second;
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
        if (!batch_->pages.contains(id) && batch_->pages.size() >= maxBatchPages)
            throw MiniSqlError(ErrorCode::Transaction, "Write batch page limit exceeded; rollback required");
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
    snapshot->startLsn = walBytes();
    batch_ = std::move(snapshot);
}
void PageFile::rollbackWriteBatch() {
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
    // 丢弃本批次已追加但未提交（无提交标记）的预备扩展，避免其阻塞其后已提交扩展的恢复。
    if (walBytes() > batch_->startLsn) {
        const auto journal = sidecar(path_, ".wal");
        std::filesystem::resize_file(journal, batch_->startLsn);
    }
    count_ = batch_->count;
    active_ = std::move(batch_->active);
    owners_ = std::move(batch_->owners);
    free_ = std::move(batch_->free);
    batch_.reset();
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
    bool publishedMarker = false;
    try {
        std::ofstream output(journal, std::ios::binary | std::ios::app);
        PageBytes header{};
        writeUnsigned(header, 0, 4, journalMagic);writeUnsigned(header, 8, 4, 1);writeUnsigned(header, 12, 4, kPageSize);
        writeUnsigned(header, 16, 8, ordered.size());writeUnsigned(header, 24, 8, count_);
        writeUnsigned(header, 32, 8, identity_[0]);writeUnsigned(header, 40, 8, identity_[1]);
        writeUnsigned(header, 48, 8, batch_->count);writeUnsigned(header, 56, 8, seq);writeUnsigned(header, 64, 8, 0);
        writeUnsigned(header, 72, 8, startLsn);writeUnsigned(header, 80, 8, startLsn);
        writeUnsigned(header, 88, 8, checkpointRecord_.present ? checkpointRecord_.walCutoffBytes : 0);seal(header);
        putPage(output, header);
        for (const auto& [id, bytes] : ordered) {
            PageBytes record{};
            writeUnsigned(record, 0, 4, recordMagic);writeUnsigned(record, 8, 8, id);
            writeUnsigned(record, 16, 4, checksum(bytes));seal(record);
            putPage(output, record);putPage(output, bytes);
        }
        output.flush();if (!output) fail("Journal prepare failed");syncFile(journal);
        notify("prepared");
        header[0]; // 已写盘（header+data），随后追加提交标记形成"已提交"。
        PageBytes marker{};
        writeUnsigned(marker, 0, 4, commitMarkerMagic);writeUnsigned(marker, 8, 8, seq);seal(marker);
        putPage(output, marker);
        output.flush();if (!output) fail("Journal commit failed");syncFile(journal);
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
    applyExtent(ordered, count_, false);
    ++committedSequence_;
    batch_.reset();
    notify("checkpointed");
}
void PageFile::checkpoint(const CheckpointOptions& options) {
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Cannot checkpoint during a write batch");
    // 检查点后整段日志回收（已提交数据均已落盘），恢复起点归零；脏页水位=已落盘页数上限。
    checkpointRecord_.present = true;
    checkpointRecord_.walCutoffBytes = 0;
    checkpointRecord_.dirtyWatermark = count_;
    dirtyWatermark_ = count_;
    checkpointRecord_.catalogVersion = options.catalogVersion;
    checkpointRecord_.indexVersion = options.indexVersion;
    checkpointRecord_.committedSequence = committedSequence_;
    checkpointJournal();
    writeCheckpointRecord();
}
void PageFile::checkpointJournal() {
    const auto journal = sidecar(path_, ".wal");
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    if (!output) fail("Cannot truncate redo journal");
    output.close();
    if (!output) fail("Cannot close redo journal");
    syncFile(journal);
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
        if (readUnsigned(header, 8, 4) != 1 || readUnsigned(header, 12, 4) != kPageSize ||
            readUnsigned(header, 4, 4) != checksum(header)) fail("STORAGE_CORRUPTION: redo header");
        const auto records = readUnsigned(header, 16, 8), finalCount = readUnsigned(header, 24, 8), baseCount = readUnsigned(header, 48, 8);
        if (records == 0 || records > maxBatchPages || baseCount == 0 || finalCount < baseCount || finalCount - baseCount > records ||
            finalCount > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("STORAGE_CORRUPTION: redo header fields");
        const std::array<std::uint64_t, 2> identity{readUnsigned(header, 32, 8), readUnsigned(header, 40, 8)};
        const auto original = readRaw(0);
        if ((identity[0] == 0 && identity[1] == 0) || identity[0] != readUnsigned(original, 24, 8) || identity[1] != readUnsigned(original, 32, 8))
            fail("STORAGE_CORRUPTION: redo database identity mismatch");
        std::map<PageId, PageBytes> pages;
        for (std::uint64_t i = 0; i < records; ++i) {
            PageBytes record{}, bytes{};
            if (!readPage(record) || !readPage(bytes)) fail("STORAGE_CORRUPTION: redo truncated");
            const auto id = readUnsigned(record, 8, 8);
            if (readUnsigned(record, 0, 4) != recordMagic || readUnsigned(record, 4, 4) != checksum(record) || id >= finalCount ||
                readUnsigned(record, 16, 4) != checksum(bytes) || !pages.emplace(id, bytes).second) fail("STORAGE_CORRUPTION: redo record");
            validatePage(bytes, id, finalCount, identity);
        }
        if (!pages.contains(0)) fail("STORAGE_CORRUPTION: redo missing file header");
        for (auto id = baseCount; id < finalCount; ++id) if (!pages.contains(id)) fail("STORAGE_CORRUPTION: redo missing allocated page");
        PageBytes marker{};
        const bool hasMarker = readPage(marker);
        if (!hasMarker) break;   // 净 EOF：预备但未提交（无提交标记）的尾部，丢弃
        if (readUnsigned(marker, 0, 4) == commitMarkerMagic && readUnsigned(marker, 8, 8) == readUnsigned(header, 56, 8)) {
            lastAppliedSeq = readUnsigned(marker, 8, 8);
            applyExtent(pages, finalCount, recovering);   // 已提交：落盘（幂等）
            has = readPage(lookahead);
        } else break;                                       // 标记缺失/不匹配：停止（其后无更优提交）
    }
    input.close();
    // 保持 LSN 语义连续：重做所达的最后提交序号在无 .ckpt（或早于日志）时也须保留，供后续提交序号递增。
    if (lastAppliedSeq > committedSequence_) committedSequence_ = lastAppliedSeq;
    // 持久化恢复所达提交序号：期刊已清空（截止归零），若不落盘，干净重启会回退到旧 .ckpt 序号并复用 LSN。
    if (lastAppliedSeq > 0) {
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
    for (const auto& [id, bytes] : pages) if (id != 0) {
        writeDiskRaw(id, bytes);
        flush();
        notify(recovering ? "recovery-page" : "applied-page");
    }
    writeDiskRaw(0, pages.at(0));flush();
    std::filesystem::resize_file(path_, finalCount * kPageSize);
    syncFile(path_);
    notify(recovering ? "recovery-synced" : "data-synced");
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
    dirtyWatermark_ = checkpointRecord_.dirtyWatermark;
    committedSequence_ = checkpointRecord_.committedSequence;
}
}
