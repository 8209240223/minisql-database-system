#include "minisql/storage/page_file.hpp"
#include "minisql/common/error.hpp"
#include <limits>
#include <algorithm>
#include <map>
#include <random>
#include <cstdlib>

namespace minisql::storage {
namespace {
constexpr std::uint32_t fileMagic = 0x4644534d;
constexpr std::uint32_t freeMagic = 0x4652534d;
constexpr std::uint32_t journalMagic = 0x4a44534d;
constexpr std::uint32_t recordMagic = 0x5244534d;
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
PageBytes getPage(std::istream& input) {
    PageBytes bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input || input.gcount() != kPageSize) fail("Journal truncated");
    return bytes;
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
    batch_ = std::move(snapshot);
}
void PageFile::rollbackWriteBatch() {
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
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
    const auto temporary = sidecar(path_, ".wal.tmp");
    try {
        writeHeader();
        PageBytes header{};
        writeUnsigned(header, 0, 4, journalMagic);writeUnsigned(header, 8, 4, 1);writeUnsigned(header, 12, 4, kPageSize);
        writeUnsigned(header, 16, 8, batch_->pages.size());writeUnsigned(header, 24, 8, count_);
        writeUnsigned(header, 32, 8, identity_[0]);writeUnsigned(header, 40, 8, identity_[1]);
        writeUnsigned(header, 48, 8, batch_->count);seal(header);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        putPage(output, header);
        const std::map<PageId, PageBytes> ordered(batch_->pages.begin(), batch_->pages.end());
        for (const auto& [id, bytes] : ordered) {
            PageBytes record{};
            writeUnsigned(record, 0, 4, recordMagic);writeUnsigned(record, 8, 8, id);
            writeUnsigned(record, 16, 4, checksum(bytes));seal(record);
            putPage(output, record);putPage(output, bytes);
        }
        output.close();if (!output) fail("Journal close failed");
        syncFile(temporary);
        lastCommitWalBytes_ = std::filesystem::file_size(temporary);
        notify("prepared");
        publishFile(temporary, journal);
        batch_->published = true;
        notify("published");
        recoverJournal(false);
        batch_.reset();
    } catch (...) {
        std::error_code error;
        if (batch_->published || (std::filesystem::exists(journal, error) && std::filesystem::file_size(journal, error) != 0)) {
            batch_->published = true;failed_ = true;
        }
        throw;
    }
}
void PageFile::checkpoint() {
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Cannot checkpoint during a write batch");
    checkpointJournal();
}
void PageFile::checkpointJournal() {
    const auto journal = sidecar(path_, ".wal");
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    if (!output) fail("Cannot truncate redo journal");
    output.close();
    if (!output) fail("Cannot close redo journal");
    syncFile(journal);
}
void PageFile::notify(std::string_view stage) {
    if (observer_) observer_(stage);
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
    std::ifstream input(journal, std::ios::binary);
    const auto header = getPage(input);
    if (readUnsigned(header, 0, 4) != journalMagic || readUnsigned(header, 8, 4) != 1 ||
        readUnsigned(header, 12, 4) != kPageSize || readUnsigned(header, 4, 4) != checksum(header)) fail("STORAGE_CORRUPTION: redo header");
    const auto records = readUnsigned(header, 16, 8), finalCount = readUnsigned(header, 24, 8), baseCount = readUnsigned(header, 48, 8);
    if (records == 0 || records > maxBatchPages || baseCount == 0 || finalCount < baseCount || finalCount - baseCount > records ||
        finalCount > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize ||
        std::filesystem::file_size(journal) != (1 + records * 2) * kPageSize ||
        std::filesystem::file_size(path_) < baseCount * kPageSize) fail("STORAGE_CORRUPTION: redo length");
    const std::array<std::uint64_t, 2> identity{readUnsigned(header, 32, 8), readUnsigned(header, 40, 8)};
    const auto original = readRaw(0);
    if ((identity[0] == 0 && identity[1] == 0) || identity[0] != readUnsigned(original, 24, 8) || identity[1] != readUnsigned(original, 32, 8))
        fail("STORAGE_CORRUPTION: redo database identity mismatch");
    std::map<PageId, PageBytes> pages;
    for (std::uint64_t i = 0; i < records; ++i) {
        const auto record = getPage(input), bytes = getPage(input);
        const auto id = readUnsigned(record, 8, 8);
        if (readUnsigned(record, 0, 4) != recordMagic || readUnsigned(record, 4, 4) != checksum(record) || id >= finalCount ||
            readUnsigned(record, 16, 4) != checksum(bytes) || readUnsigned(bytes, 4, 4) != checksum(bytes) || !pages.emplace(id, bytes).second)
            fail("STORAGE_CORRUPTION: redo record");
        if (id == 0) {
            if (readUnsigned(bytes, 0, 4) != fileMagic || readUnsigned(bytes, 8, 4) != 2 || readUnsigned(bytes, 12, 4) != kPageSize ||
                readUnsigned(bytes, 16, 8) != finalCount || readUnsigned(bytes, 24, 8) != identity[0] || readUnsigned(bytes, 32, 8) != identity[1])
                fail("STORAGE_CORRUPTION: redo file header");
        } else if (readUnsigned(bytes, 0, 4) == freeMagic) {
            if (readUnsigned(bytes, 8, 8) != id || readUnsigned(bytes, 24, 8) == 0) fail("STORAGE_CORRUPTION: redo free page");
        } else if (SlottedPage(bytes).id() != id) fail("STORAGE_CORRUPTION: redo page identity");
    }
    if (!pages.contains(0)) fail("STORAGE_CORRUPTION: redo missing file header");
    for (auto id = baseCount; id < finalCount; ++id) if (!pages.contains(id)) fail("STORAGE_CORRUPTION: redo missing allocated page");
    input.close();
    // 完整验证日志后才写数据；文件头最后写入，恢复可重复执行。
    for (const auto& [id, bytes] : pages) if (id != 0) {
        writeDiskRaw(id, bytes);
        flush();
        notify(recovering ? "recovery-page" : "applied-page");
    }
    writeDiskRaw(0, pages.at(0));flush();
    std::filesystem::resize_file(path_, finalCount * kPageSize);
    syncFile(path_);
    notify(recovering ? "recovery-synced" : "data-synced");
    checkpointJournal();
    notify("checkpointed");
}
std::vector<PageRef> PageFile::pagesFor(std::uint64_t owner) const {
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    std::vector<PageRef> result;
    for (const auto& [id, value] : owners_) if (value == owner) result.push_back({id, active_.at(id)});
    std::sort(result.begin(), result.end(), [](const PageRef& a, const PageRef& b) { return a.id < b.id; });
    return result;
}
}
