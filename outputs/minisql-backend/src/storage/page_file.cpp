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
// 文件头魔数，标记这是一个 MiniSQL 页文件。
constexpr std::uint32_t freeMagic = 0x4652534d;
// 空闲页魔数，标记这一页已经被释放。
constexpr std::uint32_t journalMagic = 0x4a44534d;
// 日志扩展头魔数。
constexpr std::uint32_t recordMagic = 0x5244534d;
// 日志中单条页记录的魔数。
constexpr std::uint32_t commitMarkerMagic = 0x434d544d;
// 提交标记魔数；只有写了它，这次日志扩展才算已提交。
constexpr std::uint32_t checkpointMagic = 0x4d595043;
// 检查点记录魔数。
constexpr std::uint32_t dwbMagic = 0x4257444d;   // 'MDWB'：双写缓冲头
constexpr std::uint32_t doubleWriteSlots = 32;   // 单次扩展最多暂存的页数
constexpr std::size_t defaultMaxBatchPages = 16384;
// 单个写批次允许修改的页数上限。
// WAL 扩展头/页记录的字段偏移与类型编号（v2 起启用记录级元数据；v1 日志仍可读）。
constexpr std::uint32_t journalVersionV1 = 1;
constexpr std::uint32_t journalVersionV2 = 2;
constexpr std::size_t hPrevLsn = 96, hRecordType = 104, hFlags = 108, hCommitLsn = 112, hUndoNextLsn = 120, hExtentLsn = 128;
constexpr std::size_t rPageId = 8, rAfterChecksum = 16, rRecordLsn = 24, rPrevRecordLsn = 32, rRecordType = 40;
constexpr std::uint32_t recordRedo = 1, recordAbort = 3;
constexpr std::size_t cLastLsn = 64, cBeginLsn = 72, cEndLsn = 80, cArchivedBytes = 88, cArchiveSegments = 96;
std::size_t maxBatchPages() {
// 读取批量页数上限，允许用环境变量覆盖。
    const auto* configured = std::getenv("MINISQL_MAX_BATCH_PAGES");
    // 取环境变量。
    if (!configured || !*configured) return defaultMaxBatchPages;
    // 没配置就用默认值。
    char* end = nullptr;
    // 解析结束位置。
    const auto parsed = std::strtoull(configured, &end, 10);
    // 按十进制解析。
    if (end && *end == '\0' && parsed > 0 && parsed <= defaultMaxBatchPages)
    // 必须是完整数字且不超过硬上限。
        return static_cast<std::size_t>(parsed);
        // 合法则采用配置值。
    return defaultMaxBatchPages;
    // 非法配置一律回退到默认值。
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
// 统一的存储错误出口。
void seal(PageBytes& bytes) { writeUnsigned(bytes, 4, 4, checksum(bytes)); }
// 写入校验和：把计算出的校验和填到偏移 4，完成“封页”。
std::filesystem::path sidecar(std::filesystem::path path, const char* suffix) { path += suffix;return path; }
// 由主文件路径派生附属文件路径，例如 xxx.pages 加 .wal 得到日志路径。
std::array<std::uint64_t, 2> newIdentity() {
// 生成一个随机数据库身份，用于校验日志与数据文件配套。
    std::random_device random;
    // 随机数源。
    std::array<std::uint64_t, 2> result{};
    // 两个 64 位分量。
    do { for (auto& part : result) part = (std::uint64_t(random()) << 32) | random(); } while (result[0] == 0 && result[1] == 0);
    // 每个分量由两次随机拼接；若两分量同时为零则重来，避免出现全零身份。
    return result;
    // 返回身份。
}
void putPage(std::ostream& output, const PageBytes& bytes) {
// 把一个完整页写入输出流，并检查写入是否成功。
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // 写入 4096 字节。
    if (!output) fail("Journal write failed");
    // 流状态异常说明写失败。
}
}

PageFile::PageFile(const std::filesystem::path& path, CommitObserver observer)
    : path_(std::filesystem::weakly_canonical(path)), observer_(std::move(observer)) {
    // 规范化路径（去掉 . 与 ..），并保存观察者。
    if (std::filesystem::exists(path_) && std::filesystem::hard_link_count(path_) > 1) fail("Hard-linked database files are not supported");
    // 硬链接会让两个路径指向同一份数据，破坏独占假设，直接拒绝。
    lock_ = std::make_unique<ExclusiveFileLock>(sidecar(path_, ".lock"));
    // 对 .lock 附属文件加独占锁，保证同一数据库只能被一个进程打开。
    const auto journal = sidecar(path_, ".wal");
    // 计算日志路径。
    const bool pendingJournal = std::filesystem::exists(journal) && std::filesystem::file_size(journal) != 0;
    // 日志存在且非空，说明上次可能有未完成的提交需要重做。
    if (!std::filesystem::exists(path_)) {
    // 数据文件不存在的情况。
        if (pendingJournal) fail("Database missing while redo journal exists");
        // 只有日志没有数据文件，属于状态不一致，拒绝启动以免误恢复。
        std::ofstream create(path_, std::ios::binary);
        // 创建空文件。
        if (!create) fail("Cannot create page file");
        // 创建失败通常是路径或权限问题。
        create.close();
        // 关闭，后面改用 fstream 打开。
    }
    auto size = std::filesystem::file_size(path_);
    // 取文件大小，用于判断是不是空文件。
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    // 以二进制读写方式打开。
    if (!file_) fail("Cannot open page file");
    // 打开失败即报错。
    loadCheckpointRecord();
    // 先读检查点记录，恢复流程要用到其中的截止位置。
    if (const char* configured = std::getenv("MINISQL_DOUBLEWRITE"); configured && std::string_view(configured) == "1")
        doubleWrite_ = true;
    if (const char* configured = std::getenv("MINISQL_GROUP_COMMIT"); configured && std::string_view(configured) == "1")
        groupCommit_ = true;    if (size == 0) {
        if (pendingJournal) fail("Database empty while redo journal exists");
        // 空文件却有日志，同样属于不一致状态。
        identity_ = newIdentity();writeHeader();flush();syncFile(path_);return;
        // 生成身份、写文件头、刷盘并同步，构造完成。
    }
    // 双写缓冲先行修复未完成的页写（提供干净的基页），随后再做日志重做。
    if (doubleWrite_) { recoverDoubleWrite();size = std::filesystem::file_size(path_); }
    if (pendingJournal) { recoverJournal(true);size = std::filesystem::file_size(path_); }
    // 有未完成日志就先重做，重做可能改变文件长度，所以重新取大小。
    if (size < kPageSize || size % kPageSize != 0) fail("STORAGE_CORRUPTION: file length");
    // 文件长度必须是页大小整数倍且至少一页。
    const auto header = readRaw(0);
    // 读第 0 页文件头。
    const auto version = readUnsigned(header, 8, 4);
    // 文件格式版本。
    if (readUnsigned(header, 0, 4) != fileMagic || (version != 1 && version != 2) ||
        readUnsigned(header, 12, 4) != kPageSize || readUnsigned(header, 4, 4) != checksum(header))
        // 魔数、版本、页大小、校验和四项都要对。
        fail("STORAGE_CORRUPTION: file header");
    count_ = readUnsigned(header, 16, 8);
    // 从文件头读出页总数。
    if (version == 2) {
    // 版本 2 才有数据库身份字段。
        identity_ = {readUnsigned(header, 24, 8), readUnsigned(header, 32, 8)};
        // 读出身份。
        if (identity_[0] == 0 && identity_[1] == 0) fail("STORAGE_CORRUPTION: database identity");
        // 全零身份不合法。
    }
    if (count_ != size / kPageSize) fail("STORAGE_CORRUPTION: page count");
    // 头里记录的页数必须与实际文件长度一致。
    for (PageId id = 1; id < count_; ++id) {
    // 从第 1 页开始逐页扫描，重建内存中的页表。
        const auto bytes = readRaw(id);
        // 读这一页。
        if (readUnsigned(bytes, 0, 4) == freeMagic) {
        // 是空闲页标记。
            if (readUnsigned(bytes, 4, 4) != checksum(bytes) || readUnsigned(bytes, 8, 8) != id || readUnsigned(bytes, 24, 8) == 0)
                // 空闲页同样要有正确校验和、页号与代数。
                fail("STORAGE_CORRUPTION: free page");
            free_.push_back({id, readUnsigned(bytes, 24, 8)});
            // 加入空闲页列表，并记住它的代数。
        } else {
        // 否则应当是一张正常的数据页。
            const SlottedPage page(bytes);
            // 构造页对象，内部会校验校验和与结构。
            if (page.id() != id) fail("STORAGE_CORRUPTION: page identity");
            // 页内记录的页号必须与它在文件中的位置一致。
            active_.emplace(id, page.generation());
            // 登记为有效页。
            owners_.emplace(id, page.owner());
            // 登记归属者。
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
// 读原始页字节：先看写批次的暂存区，再读文件。
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    // 故障状态下禁止继续读写。
    if (batch_) {
    // 处于写批次时，暂存区里的版本比磁盘新。
        const auto found = batch_->pages.find(id);
        // 查暂存区。
        if (found != batch_->pages.end()) return found->second;
        // 命中就直接返回内存里的内容。
    } else if (const auto pending = pendingPages_.find(id); pending != pendingPages_.end()) {
        // group commit 尚未应用的扩展：读必须看到已提交的新内容。
        return pending->second;
    }
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    // 页号乘页大小可能超出流偏移能表示的范围。
    PageBytes bytes{};
    // 输出缓冲。
    file_.clear();
    // 清除流上的错误标志，允许重试。
    file_.seekg(static_cast<std::streamoff>(id * kPageSize));
    // 定位到该页的字节偏移：页号乘 4096。
    file_.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    // 读满一页。
    if (!file_ || file_.gcount() != kPageSize) { ++ioStats_.errors; failed_ = true; fail("Page short read or I/O failure"); }
    // 读失败或没读满一页都属于 IO 错误，并让页文件进入故障状态。
    ++ioStats_.reads;
    // 读计数加一。
    return bytes;
    // 返回读到的字节。
}
void PageFile::writeRaw(PageId id, const PageBytes& bytes) {
// 写原始页字节：批次内进暂存区，否则直接落盘。
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    // 故障状态检查。
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    // 偏移溢出检查。
    if (batch_) {
    // 写批次分支。
        if (!batch_->pages.contains(id) && batch_->pages.size() >= maxBatchPages())
        // 新页加入暂存区之前先检查批次容量。
            throw MiniSqlError(ErrorCode::Transaction, "Write batch page limit exceeded; rollback required");
            // 超出上限属于事务级问题，要求回滚。
        batch_->touched = true;
        batch_->pages.insert_or_assign(id, bytes);return;
        // 写入或覆盖暂存区内容，不碰磁盘。
    }
    writeDiskRaw(id, bytes);
    // 不在批次中就直接写磁盘。
}
void PageFile::writeDiskRaw(PageId id, const PageBytes& bytes) {
// 真正把一页写到磁盘文件。
    requireHealthy();
    // 健康检查。
    if (id > static_cast<PageId>(std::numeric_limits<std::streamoff>::max()) / kPageSize) fail("Page offset overflow");
    // 偏移溢出检查。
    file_.clear();
    // 清除错误标志。
    file_.seekp(static_cast<std::streamoff>(id * kPageSize));
    // 定位到该页偏移。
    file_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // 写入一页。
    if (!file_) { ++ioStats_.errors; failed_ = true; fail("Page write I/O failure"); }
    // 写失败即标记故障。
    ++ioStats_.writes;
    // 写计数加一。
}
void PageFile::writeHeader() {
// 写第 0 页的文件头。
    PageBytes header{};
    // 头部缓冲。
    writeUnsigned(header, 0, 4, fileMagic);
    // 偏移 0 魔数。
    writeUnsigned(header, 8, 4, identity_[0] == 0 && identity_[1] == 0 ? 1 : 2);
    // 偏移 8 格式版本：有身份则为 2，否则为 1。
    writeUnsigned(header, 12, 4, kPageSize);
    // 偏移 12 页大小。
    writeUnsigned(header, 16, 8, count_);
    // 偏移 16 页总数。
    writeUnsigned(header, 24, 8, identity_[0]);
    // 偏移 24 身份第一分量。
    writeUnsigned(header, 32, 8, identity_[1]);
    // 偏移 32 身份第二分量。
    seal(header);
    // 写校验和。
    writeRaw(0, header);
    // 用统一的写入口落盘，保证批次语义一致。
}
void PageFile::requireActive(PageRef ref) const {
// 校验引用指向有效页，且代数匹配。
    const auto found = active_.find(ref.id);
    // 查有效页表。
    if (found == active_.end() || found->second != ref.generation) fail("STALE_PAGE_ID");
    // 不存在或代数不符都说明引用过期。
}
PageRef PageFile::allocate(std::uint64_t owner) {
// 分配一页：优先复用空闲页，否则追加新页。
    PageRef ref{count_, 1};
    // 默认在文件末尾新增一页，代数从 1 开始。
    const bool reused = !free_.empty();
    // 空闲列表非空就复用。
    if (reused) {
    // 复用分支。
        ref = free_.back();
        // 取最后一个空闲页。
        if (ref.generation == std::numeric_limits<std::uint64_t>::max()) fail("Page generation exhausted");
        // 代数已经到上限，无法再区分新旧引用。
        ++ref.generation;
        // 代数加一，让旧引用立即失效。
    }
    writeRaw(ref.id, SlottedPage(ref.id, owner, ref.generation).serialize());
    // 写一张全新的空页，归属者设为调用方。
    if (!reused) { ++count_; writeHeader(); }
    // 新增页时要更新页总数与文件头。
    flush();
    // 刷盘，确保分配结果持久化。
    if (reused) free_.pop_back();
    // 从空闲列表移除。
    active_.emplace(ref.id, ref.generation);
    // 登记为有效页。
    owners_.emplace(ref.id, owner);
    // 登记归属者。
    return ref;
    // 返回页引用。
}
SlottedPage PageFile::read(PageRef ref) {
// 读一页并做三重校验。
    requireActive(ref);
    // 引用必须有效。
    SlottedPage page(readRaw(ref.id));
    // 读字节构造页对象。
    if (page.id() != ref.id || page.generation() != ref.generation || page.owner() != owners_.at(ref.id)) fail("STORAGE_CORRUPTION: page ownership");
    // 页内页号、代数、归属者都要与内存记录一致。
    return page;
    // 返回页。
}
void PageFile::write(const SlottedPage& page) {
// 写回一页。
    requireActive({page.id(), page.generation()});
    // 页必须是有效页。
    if (page.owner() != owners_.at(page.id())) fail("Page owner mismatch");
    // 归属者不能改变。
    writeRaw(page.id(), page.serialize());
    // 序列化后写入。
}
void PageFile::release(PageRef ref) {
// 释放一页：写入空闲页标记并登记到空闲列表。
    requireActive(ref);
    // 引用必须有效。
    PageBytes bytes{};
    // 空白页缓冲。
    writeUnsigned(bytes, 0, 4, freeMagic);
    // 偏移 0 写空闲页魔数。
    writeUnsigned(bytes, 8, 8, ref.id);
    // 偏移 8 写页号。
    writeUnsigned(bytes, 24, 8, ref.generation);
    // 偏移 24 写代数，供将来复用时递增。
    seal(bytes);
    // 写校验和。
    writeRaw(ref.id, bytes);
    // 落盘。
    flush();
    // 刷盘确保释放生效。
    active_.erase(ref.id);
    // 从有效页表移除。
    owners_.erase(ref.id);
    // 从归属表移除。
    free_.push_back(ref);
    // 加入空闲列表。
}
void PageFile::flush() {
// 刷文件缓冲。
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    // 故障状态检查。
    file_.flush();
    // 把流缓冲交给操作系统。
    if (!file_) { failed_ = true; fail("Page flush I/O failure"); }
    // 刷失败即进入故障状态。
}
void PageFile::beginWriteBatch() {
// 开始写批次：先刷盘，再建立一份完整的内存快照。
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Nested write batches are not supported");
    // 禁止嵌套批次。
    flush();
    // 把之前的改动刷干净。
    ensureIdentity();
    // 老格式文件在这里补上身份，保证日志可校验。
    syncFile(path_);
    // 同步到物理磁盘，作为批次的起点。
    auto snapshot = std::make_unique<WriteBatch>();
    // 新建批次对象。
    snapshot->count = count_;
    // 记录页总数。
    snapshot->active = active_;
    // 记录有效页表。
    snapshot->owners = owners_;
    // 记录归属表。
    snapshot->free = free_;
    // 记录空闲页列表。
    snapshot->txId = ++txSequence_;
    // 分配事务号。
    snapshot->startLsn = walBytes();
    // 记录日志当前长度，回滚时截断到此处。
    snapshot->prevLsn = lastExtentLsn_;
    snapshot->extentLsn = nextLsn_;
    batch_ = std::move(snapshot);
    // 进入批次状态。
}
void PageFile::rollbackWriteBatch() {
// 回滚写批次：截断日志并还原内存快照。
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    // 没有批次无法回滚。
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
    // 已经写过提交标记就不能回滚，只能靠重开数据库走恢复。
    // 丢弃本批次已追加但未提交（无提交标记）的预备扩展，避免其阻碍其后已提交扩展的恢复。
    // 丢弃本批次已追加但未提交（无提交标记）的预备扩展，避免其阻塞其后已提交扩展的恢复。
    const auto journal = sidecar(path_, ".wal");
    const auto startOffset = batch_->startLsn;
    if (walBytes() > startOffset) std::filesystem::resize_file(journal, startOffset);
    const bool touched = batch_->touched;
    const auto txId = batch_->txId, prevLsn = batch_->prevLsn, extentLsn = batch_->extentLsn, baseCount = batch_->count;
    count_ = batch_->count;
    // 还原页总数。
    active_ = std::move(batch_->active);
    // 还原有效页表。
    owners_ = std::move(batch_->owners);
    // 还原归属表。
    free_ = std::move(batch_->free);
    // 还原空闲列表。
    batch_.reset();
    // 退出批次状态。
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
// 健康检查，供底层写路径调用。
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    // 一旦标记故障，必须重开数据库才能继续。
}
void PageFile::ensureIdentity() {
// 旧格式文件升级：补上数据库身份并重写整个文件。
    if (identity_[0] != 0 || identity_[1] != 0) return;
    // 已有身份就不需要升级。
    const auto identity = newIdentity();
    // 生成新身份。
    const auto temporary = sidecar(path_, ".upgrade.tmp");
    // 先写临时文件，成功后再原子替换。
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    // 打开临时文件。
    auto header = readRaw(0);
    // 读原文件头。
    writeUnsigned(header, 8, 4, 2);writeUnsigned(header, 24, 8, identity[0]);writeUnsigned(header, 32, 8, identity[1]);seal(header);
    // 版本改为 2，写入身份，重新计算校验和。
    putPage(output, header);
    // 先写头部。
    for (PageId id = 1; id < count_; ++id) putPage(output, readRaw(id));
    // 依次复制其余页。
    output.close();if (!output) fail("Legacy header upgrade write failed");
    // 关闭并检查写入是否成功。
    syncFile(temporary);
    // 把临时文件刷到磁盘。
    file_.close();
    // 关闭原文件，准备替换。
    try { publishFile(temporary, path_); }
    // 原子替换。
    catch (...) { failed_ = true;throw; }
    // 替换失败说明文件状态可疑，标记故障并上抛。
    file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    // 重新打开升级后的文件。
    if (!file_) { failed_ = true;fail("Cannot reopen upgraded database"); }
    // 打不开就进入故障状态。
    identity_ = identity;
    // 记住新身份。
}
void PageFile::commitWriteBatch() {
// 提交写批次：先写日志与提交标记，再把数据页落盘。
    requireHealthy();
    // 健康检查。
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    // 没有批次不能提交。
    if (batch_->pages.empty()) { lastCommitWalBytes_ = 0; batch_.reset();return; }
    // 本批次没改动任何页，直接结束，不必写日志。
    const auto journal = sidecar(path_, ".wal");
    // 日志路径。
    writeHeader();
    // 先把最新的文件头写出去（批次内它可能已进暂存区）。
    const std::map<PageId, PageBytes> ordered(batch_->pages.begin(), batch_->pages.end());
    // 按页号排序，保证日志内容顺序确定、可复现。
    const auto seq = committedSequence_ + 1;
    // 本次提交的序号。
    const auto startLsn = walBytes();
    // 本次扩展在日志中的起始偏移。
    const auto extentLsn = batch_->extentLsn;
    const auto prevLsn = batch_->prevLsn;
    // 扩展字节 = 头(1 页) + 每条(记录 1 页 + 数据 1 页) + 提交标记(1 页)。
    // commitLsn 指向提交标记页；endLsn 为本次扩展末尾（不含）偏移。
    const auto commitLsn = startLsn + (2 * ordered.size() + 1) * kPageSize;
    const auto endLsn = startLsn + 2 * (ordered.size() + 1) * kPageSize;
    bool publishedMarker = false;
    // 记录提交标记是否已经写出，异常时据此判断能否回滚。
    try {
        std::ofstream output(journal, std::ios::binary | std::ios::app);
        // 以追加方式打开日志。
        PageBytes header{};
        // 扩展头缓冲。
        writeUnsigned(header, 0, 4, journalMagic);writeUnsigned(header, 8, 4, journalVersionV2);writeUnsigned(header, 12, 4, kPageSize);
        writeUnsigned(header, 16, 8, ordered.size());writeUnsigned(header, 24, 8, count_);
        // 写本扩展包含的页数与最终页总数。
        writeUnsigned(header, 32, 8, identity_[0]);writeUnsigned(header, 40, 8, identity_[1]);
        // 写数据库身份，恢复时用来确认日志与数据配套。
        writeUnsigned(header, 48, 8, batch_->count);writeUnsigned(header, 56, 8, seq);writeUnsigned(header, 64, 8, batch_->txId);
        // 写批次起始页数、提交序号与事务号。
        // 扩展字节 = 头（1 页）+ 每条（记录 1 页 + 数据 1 页）+ 提交标记（1 页）；endLsn 为本次扩展末尾偏移。
        // 扩展字节 = 头(1 页) + 每条(记录 1 页 + 数据 1 页) + 提交标记(1 页)；endLsn 为本次扩展末尾偏移。
        writeUnsigned(header, 72, 8, startLsn);writeUnsigned(header, 80, 8, endLsn);
        // 写起始与结束偏移。
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
        // 落盘扩展头。
        std::uint64_t recordLsn = startLsn + kPageSize;
        for (const auto& [id, bytes] : ordered) {
        // 逐页写记录。
            const auto previous = pageLsn_.contains(id) ? pageLsn_.at(id) : 0;
            PageBytes record{};
            // 记录页缓冲。
            writeUnsigned(record, 0, 4, recordMagic);writeUnsigned(record, rPageId, 8, id);
            writeUnsigned(record, rAfterChecksum, 4, checksum(bytes));
            writeUnsigned(record, rRecordLsn, 8, recordLsn);
            writeUnsigned(record, rPrevRecordLsn, 8, previous);
            writeUnsigned(record, rRecordType, 4, recordRedo);
            seal(record);
            putPage(output, record);putPage(output, bytes);
            // 先写记录页，再写数据页本身。
            pageLsn_.insert_or_assign(id, recordLsn);
            ++recordedPages_;
            recordLsn += 2 * kPageSize;
        }
        output.flush();if (!output) fail("Journal prepare failed");
        if (!groupCommit_) syncFile(journal);
        notify("prepared");
        // 通知观察者，可以在此注入崩溃，测试“未提交”场景。
        header[0]; // 已写盘（header+data），随后追加提交标记形成"已提交"。
        PageBytes marker{};
        // 提交标记缓冲。
        writeUnsigned(marker, 0, 4, commitMarkerMagic);writeUnsigned(marker, 8, 8, seq);seal(marker);
        // 写魔数与提交序号。
        putPage(output, marker);
        // 追加提交标记；从此这次扩展被视为已提交。
        output.flush();if (!output) fail("Journal commit failed");
        if (!groupCommit_) syncFile(journal);
        publishedMarker = true;
        // 记录标记已写出。
        notify("published");
        // 通知观察者，可注入崩溃，测试“已提交未落盘”场景。
        output.close();if (!output) fail("Journal close failed");
        // 关闭日志文件并检查。
        lastCommitWalBytes_ = walBytes() - startLsn;
        // 记录本次提交写入的日志字节数。
        batch_->published = true;
        // 标记批次已越过提交点。
    } catch (...) {
        if (publishedMarker) { batch_->published = true;failed_ = true; }
        // 已经写过提交标记就不能假装回滚，只能标记故障等重启恢复。
        lastCommitWalBytes_ = walBytes() - startLsn;
        // 无论成功失败都记录日志增量，便于排查。
        throw;
        // 继续上抛。
    }
    // 数据即刻落盘保持 DB 现行（日志仅作 REDO，恢复幂等），此时提交点已越过，可安全推进提交序号。
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
    // 把数据页真正写到主文件。
    batch_.reset();
    // 退出批次状态。
    notify("checkpointed");
    // 通知观察者提交完成，可注入崩溃，测试“已落盘”场景。
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
// 取当前保存点。
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    // 只能在批次内建保存点。
    return {count_, active_, owners_, free_, batch_->pages, batch_->published};
    // 把当前全部状态复制成一份快照。
}
void PageFile::restoreSavepoint(const PageFileSavepoint& snapshot) {
// 还原到保存点。
    if (!batch_) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    // 必须在批次内。
    if (batch_->published) throw MiniSqlError(ErrorCode::Transaction, "Commit point passed; reopen for recovery");
    // 已提交就不能再回滚。
    count_ = snapshot.count;
    // 还原页总数。
    active_ = snapshot.active;
    // 还原有效页表。
    owners_ = snapshot.owners;
    // 还原归属表。
    free_ = snapshot.free;
    // 还原空闲列表。
    batch_->pages = snapshot.pages;
    // 还原暂存内容。
    batch_->published = snapshot.published;
    // 还原提交标志。
}
void PageFile::checkpoint(const CheckpointOptions& options) {
// 执行检查点：把已落盘数据确定下来，并清空日志。
    if (batch_) throw MiniSqlError(ErrorCode::Transaction, "Cannot checkpoint during a write batch");
    // 批次进行中不允许检查点。
    // 检查点后整段日志回收（已提交数据均已落盘），恢复起点归零；脏页水位=已落盘页数上限。
    // 检查点前先把 group commit 的待同步扩展落盘，否则会截断尚未应用的日志。
    syncJournalGroup();
    const bool fuzzy = options.fuzzy || enabledFlag("MINISQL_FUZZY_CHECKPOINT");
    const bool archive = options.archive || enabledFlag("MINISQL_ARCHIVE_WAL");
    const auto watermark = nextLsn_ > 0 ? nextLsn_ - 1 : 0;
    checkpointRecord_.present = true;
    // 标记已有检查点记录。
    checkpointRecord_.dirtyWatermark = count_;
    // 主文件已确定有效的页数上限就是当前页数。
    dirtyWatermark_ = count_;
    // 同步内存中的水位。
    checkpointRecord_.catalogVersion = options.catalogVersion;
    // 记录目录版本。
    checkpointRecord_.indexVersion = options.indexVersion;
    // 记录索引版本。
    checkpointRecord_.committedSequence = committedSequence_;
    // 记录当前提交序号。
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
    // 落盘检查点记录。
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
// 把日志文件清空。
    const auto journal = sidecar(path_, ".wal");
    // 日志路径。
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    // 以截断方式打开，等于清空。
    if (!output) fail("Cannot truncate redo journal");
    // 打不开就报错。
    output.close();
    // 关闭。
    if (!output) fail("Cannot close redo journal");
    // 关闭失败也要报错。
    syncFile(journal);
    // 把截断结果刷到磁盘。
}

void PageFile::copyTo(const std::filesystem::path& destination) {
// 复制数据库文件及其 sidecar，用于备份或快照。
    requireHealthy();
    // 健康检查。
    // 快照必须包含尚未应用的 group commit 扩展，先同步整组。
    syncJournalGroup();
    if (destination.empty()) fail("Snapshot destination is empty");
    // 目标路径不能为空。
    std::filesystem::create_directories(destination.parent_path());
    // 建立目标目录。
    if (std::filesystem::exists(destination)) fail("Snapshot destination already exists");
    // 目标已存在时拒绝覆盖，避免误删。
    std::filesystem::copy_file(path_, destination);
    // 复制主数据文件。
    if (const auto journal = sidecar(path_, ".wal"); std::filesystem::exists(journal)) {
    // 有日志就一并复制。
        std::filesystem::copy_file(journal, sidecar(destination, ".wal"));
        // 复制日志到目标同名 sidecar。
    }
    if (const auto ckpt = sidecar(path_, ".ckpt"); std::filesystem::exists(ckpt)) {
    // 有检查点记录也一并复制。
        std::filesystem::copy_file(ckpt, sidecar(destination, ".ckpt"));
        // 复制检查点记录。
    }
}
void PageFile::notify(std::string_view stage, bool allowCrash) {
// 通知观察者当前阶段，并可选地按环境变量触发崩溃。
    if (observer_) observer_(stage);
    // 回调观察者。
    if (!allowCrash) return;
    // 不允许崩溃时到这里结束。
    const auto configured = std::getenv("MINISQL_CRASH_AT");
    // 读取故障注入环境变量。
    if (configured && std::string_view(configured) == stage) std::_Exit(77);
    // 阶段名匹配就立即退出进程，退出码 77 供测试识别。
}
std::uint64_t PageFile::walBytes() const {
// 取日志文件当前字节数。
    std::error_code error;
    // 用于接收错误码。
    const auto journal = sidecar(path_, ".wal");
    // 日志路径。
    if (!std::filesystem::exists(journal, error)) {
    // 日志不存在。
        if (error) fail("Cannot inspect redo journal");
        // 查询出错说明路径有问题。
        return 0;
        // 不存在按零字节处理。
    }
    const auto size = std::filesystem::file_size(journal, error);
    // 取文件大小。
    if (error) fail("Cannot inspect redo journal");
    // 取大小失败即报错。
    return size;
    // 返回字节数。
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
// 重做日志：把已提交但未落盘的扩展逐个重新应用。
    const auto journal = sidecar(path_, ".wal");
    // 日志路径。
    if (!std::filesystem::exists(journal)) return;
    // 没有日志就无需恢复。
    std::ifstream input(journal, std::ios::binary);
    // 打开日志。
    if (!input) fail("Cannot open redo journal");
    // 打不开则报错。
    const auto size = std::filesystem::file_size(journal);
    // 日志大小。
    if (size == 0) return;
    // 空日志直接返回。
    // 非零截止位置重做：若 .ckpt 记录了本次日志的截止字节且日志保留该前缀，则跳过已落盘前缀，仅重做其后的已提交扩展。
    // 非零截止位置重做：若 .ckpt 记录了本次日志的截止字节且日志保留该前缀，则跳过已落盘前缀，仅重做其后的已提交扩展。
    std::uint64_t offset = checkpointRecord_.present ? checkpointRecord_.walCutoffBytes : 0;
    // 起始偏移由检查点记录决定。
    if (offset > size || offset % kPageSize != 0) offset = 0;
    // 偏移越界或不是页对齐就回退到从头开始。
    if (offset > 0) input.seekg(static_cast<std::streamoff>(offset));
    // 定位到起点。
    auto readPage = [&](PageBytes& out) -> bool {
    // 内部小工具：从日志读一个完整页。
        out = PageBytes{};
        // 清空输出。
        input.clear();
        // 清除流错误标志。
        input.read(reinterpret_cast<char*>(out.data()), kPageSize);
        // 读一页。
        const auto got = input.gcount();
        // 实际读到的字节数。
        if (got == 0) return false;                                        // 页边界处的干净 EOF（未提交尾或无后续）
        // 读不到任何字节，说明日志正常结束。
        if (got != kPageSize) { ++ioStats_.errors;failed_ = true;fail("STORAGE_CORRUPTION: redo truncated"); }
        // 只读到半页说明日志被截断，属于损坏。
        return true;
        // 读到完整一页。
    };
    const auto validatePage = [&](const PageBytes& bytes, PageId id, std::uint64_t finalCount, const std::array<std::uint64_t, 2>& identity) {
    // 校验日志里的一页是否与头部声明一致。
        // 页自身校验和必须先比：checksum() 覆盖页头/槽目录/行数据但跳过校验和字段
        // 本身（见 page.cpp），因此"只改校验和字段"的损坏只有显式比较才能发现。
        // 修复前页 0 这条路径不校验校验和：损坏的日志页会通过验证、被 applyExtent
        // 写进数据文件，随后在构造函数里以误导性的 file header 报错，而数据文件
        // 已经被改动——违反了"损坏日志必须受控处理、不得留下部分应用"的要求。
        if (readUnsigned(bytes, 4, 4) != checksum(bytes)) fail("STORAGE_CORRUPTION: redo page checksum");
        if (id == 0) {
        // 第 0 页是文件头。
            if (readUnsigned(bytes, 0, 4) != fileMagic || readUnsigned(bytes, 8, 4) != 2 || readUnsigned(bytes, 12, 4) != kPageSize ||
                readUnsigned(bytes, 16, 8) != finalCount || readUnsigned(bytes, 24, 8) != identity[0] || readUnsigned(bytes, 32, 8) != identity[1])
                // 魔数、版本、页大小、页总数、身份都要吻合。
                fail("STORAGE_CORRUPTION: redo file header");
        } else if (readUnsigned(bytes, 0, 4) == freeMagic) {
        // 空闲页分支。
            if (readUnsigned(bytes, 8, 8) != id || readUnsigned(bytes, 24, 8) == 0) fail("STORAGE_CORRUPTION: redo free page");
            // 空闲页的页号要正确、代数不能为零。
        } else if (SlottedPage(bytes).id() != id) fail("STORAGE_CORRUPTION: redo page identity");
        // 普通数据页的内嵌页号要与目标页号一致。
    };
    PageBytes lookahead;
    // 预读的下一页。
    bool has = readPage(lookahead);
    // 先读一页作为循环起点。
    std::uint64_t lastAppliedSeq = 0;   // 本次重做最后成功应用扩展的提交序号（LSN），用于恢复后保持序号连续
    // 记录最后应用的提交序号。
    while (has) {
    // 逐个日志扩展处理。
        const auto header = lookahead;
        // 当前扩展头。
        if (readUnsigned(header, 0, 4) != journalMagic) break;   // 遍历到异常/标记残留处停止
        // 魔数不符说明后面不再是有效扩展，停止。
        const auto version = readUnsigned(header, 8, 4);
        if ((version != journalVersionV1 && version != journalVersionV2) || readUnsigned(header, 12, 4) != kPageSize ||
            readUnsigned(header, 4, 4) != checksum(header)) fail("STORAGE_CORRUPTION: redo header");
            // 版本、页大小、校验和任一不对就是损坏。
        const auto records = readUnsigned(header, 16, 8), finalCount = readUnsigned(header, 24, 8), baseCount = readUnsigned(header, 48, 8);
        // 读出记录数、最终页数与批次起始页数。
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
            // 各字段之间必须自洽，且不能超出可表示范围。
        const std::array<std::uint64_t, 2> identity{readUnsigned(header, 32, 8), readUnsigned(header, 40, 8)};
        // 读出日志里记录的数据库身份。
        const auto original = readRaw(0);
        // 读当前数据文件的头部。
        if ((identity[0] == 0 && identity[1] == 0) || identity[0] != readUnsigned(original, 24, 8) || identity[1] != readUnsigned(original, 32, 8))
            // 身份不能为零，且必须与数据文件一致。
            fail("STORAGE_CORRUPTION: redo database identity mismatch");
        std::map<PageId, PageBytes> pages;
        // 本扩展涉及的页。
        std::map<PageId, std::uint64_t> recordLsns;   // 仅在提交确认后并入页级 LSN 链
        for (std::uint64_t i = 0; i < records; ++i) {
        // 逐条记录读取。
            PageBytes record{}, bytes{};
            // 记录页与数据页缓冲。
            if (!readPage(record) || !readPage(bytes)) fail("STORAGE_CORRUPTION: redo truncated");
            // 两条都必须完整读到。
            const auto id = readUnsigned(record, rPageId, 8);
            if (readUnsigned(record, 0, 4) != recordMagic || readUnsigned(record, 4, 4) != checksum(record) || id >= finalCount ||
                readUnsigned(record, rAfterChecksum, 4) != checksum(bytes) || !pages.emplace(id, bytes).second) fail("STORAGE_CORRUPTION: redo record");
            validatePage(bytes, id, finalCount, identity);
            // 再按页类型做内容校验。
            if (version >= journalVersionV2) recordLsns.insert_or_assign(id, readUnsigned(record, rRecordLsn, 8));
        }
        if (!pages.contains(0)) fail("STORAGE_CORRUPTION: redo missing file header");
        // 每个扩展都必须包含最新的文件头。
        for (auto id = baseCount; id < finalCount; ++id) if (!pages.contains(id)) fail("STORAGE_CORRUPTION: redo missing allocated page");
        // 新分配的页必须全部出现在扩展里，否则会出现空洞。
        PageBytes marker{};
        // 提交标记缓冲。
        const bool hasMarker = readPage(marker);
        // 尝试读提交标记。
        if (!hasMarker) break;   // 净 EOF：预备但未提交（无提交标记）的尾部，丢弃
        // 到这里就结束，说明这次扩展没写完也没提交，丢弃即可。
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
    // 关闭日志。
    // 保持 LSN 语义连续：重做所达的最后提交序号在无 .ckpt（或早于日志）时也须保留，供后续提交序号递增。
    // 保持 LSN 语义连续：重做所达的最后提交序号在无 .ckpt（或早于日志）时也须保留，供后续提交序号递增。
    if (lastAppliedSeq > committedSequence_) committedSequence_ = lastAppliedSeq;
    // 提交序号只能前进，不能后退。
    // 持久化恢复所达提交序号：期刊已清空（截止归零），若不落盘，干净重启会回退到旧 .ckpt 序号并复用 LSN。
    // 逻辑 LSN 必须跨日志截断保持单调：把已消耗到的水位随检查点记录持久化。
    checkpointRecord_.walLsn = std::max(checkpointRecord_.walLsn, nextLsn_ > 0 ? nextLsn_ - 1 : 0);
    // 持久化恢复所达提交序号：期刊已清空（截止归零），若不落盘，干净重启会回退到旧 .ckpt 序号并复用 LSN。
    if (lastAppliedSeq > 0 || nextLsn_ > 1) {
        checkpointRecord_.present = true;
        // 标记检查点记录存在。
        checkpointRecord_.walCutoffBytes = 0;
        // 日志即将清空，截止位置归零。
        checkpointRecord_.committedSequence = committedSequence_;
        // 记录推进后的提交序号。
        writeCheckpointRecord();
        // 落盘。
    }
    // 日志回收：已提交数据均已落盘且无未结束资源，整段日志可截断。
    // 日志回收：已提交数据均已落盘且无未结束资源，整段日志可截止。
    checkpointJournal();
    // 清空日志。
    notify("checkpointed", false);
    // 通知观察者恢复完成，此处不允许注入崩溃。
}
void PageFile::applyExtent(const std::map<PageId, PageBytes>& pages, std::uint64_t finalCount, bool recovering) {
// 把一批页写到主文件，并调整文件长度。
    // 双写缓冲：把本扩展的页先落入 .dwb 并同步，主文件页写未完成时仍可修复。
    if (!recovering) {
        stageDoubleWrite(pages);
        notify("doublewrite-staged");
    }
    for (const auto& [id, bytes] : pages) if (id != 0) {
    // 第 0 页留到最后写，先写数据页。
        writeDiskRaw(id, bytes);
        // 写这一页。
        flush();
        // 立即刷盘，保证顺序。
        notify(recovering ? "recovery-page" : "applied-page");
        // 通知观察者；恢复与正常提交用不同的阶段名。
    }
    writeDiskRaw(0, pages.at(0));flush();
    // 最后写文件头并刷盘。
    std::filesystem::resize_file(path_, finalCount * kPageSize);
    // 把文件长度调整到最终页数。
    syncFile(path_);
    // 同步到物理磁盘。
    notify(recovering ? "recovery-synced" : "data-synced");
    // 通知观察者数据已同步。
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
// 列出某个归属者占用的全部页。
    if (failed_) fail("Page file disabled after I/O failure; reopen required");
    // 故障检查。
    std::vector<PageRef> result;
    // 结果集合。
    for (const auto& [id, value] : owners_) if (value == owner) result.push_back({id, active_.at(id)});
    // 遍历归属表，匹配就取出页号与代数。
    std::sort(result.begin(), result.end(), [](const PageRef& a, const PageRef& b) { return a.id < b.id; });
    // 按页号升序排列，保证扫描顺序稳定。
    return result;
    // 返回列表。
}
void PageFile::writeCheckpointRecord() {
// 写 .ckpt 检查点记录。
    const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
    // 取当前时间。
    checkpointRecord_.timestampMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    // 转成毫秒时间戳写入记录。
    PageBytes bytes{};
    // 记录缓冲，固定一页。
    writeUnsigned(bytes, 0, 4, checkpointMagic);
    // 写魔数。
    writeUnsigned(bytes, 8, 4, 1);
    // 写版本。
    writeUnsigned(bytes, 16, 8, checkpointRecord_.walCutoffBytes);
    // 写日志截止位置。
    writeUnsigned(bytes, 24, 8, checkpointRecord_.dirtyWatermark);
    // 写脏页水位。
    writeUnsigned(bytes, 32, 8, checkpointRecord_.catalogVersion);
    // 写目录版本。
    writeUnsigned(bytes, 40, 8, checkpointRecord_.indexVersion);
    // 写索引版本。
    writeUnsigned(bytes, 48, 8, checkpointRecord_.committedSequence);
    // 写提交序号。
    writeUnsigned(bytes, 56, 8, checkpointRecord_.timestampMs);
    // 写时间戳。
    writeUnsigned(bytes, cLastLsn, 8, checkpointRecord_.walLsn);
    writeUnsigned(bytes, cBeginLsn, 8, checkpointRecord_.checkpointBeginLsn);
    writeUnsigned(bytes, cEndLsn, 8, checkpointRecord_.checkpointEndLsn);
    writeUnsigned(bytes, cArchivedBytes, 8, checkpointRecord_.archivedBytes);
    writeUnsigned(bytes, cArchiveSegments, 8, checkpointRecord_.archiveSegments);
    writeUnsigned(bytes, 4, 4, checksum(bytes));
    // 最后写校验和。
    const auto ckpt = sidecar(path_, ".ckpt");
    // 检查点文件路径。
    std::ofstream output(ckpt, std::ios::binary | std::ios::trunc);
    // 覆盖写。
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // 写入一页。
    if (!output) fail("Checkpoint record write failed");
    // 写失败即报错。
    output.close();
    // 关闭。
    if (!output) fail("Cannot close checkpoint record");
    // 关闭失败也要报错。
    syncFile(ckpt);
    // 刷到磁盘。
}
void PageFile::loadCheckpointRecord() {
// 读 .ckpt 检查点记录。
    const auto ckpt = sidecar(path_, ".ckpt");
    // 检查点文件路径。
    if (!std::filesystem::exists(ckpt)) return;
    // 不存在说明还没做过检查点。
    if (std::filesystem::file_size(ckpt) != kPageSize) fail("STORAGE_CORRUPTION: checkpoint record");
    // 大小必须正好一页。
    std::ifstream input(ckpt, std::ios::binary);
    // 打开。
    PageBytes bytes{};
    // 缓冲。
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    // 读一页。
    if (!input || input.gcount() != kPageSize || readUnsigned(bytes, 0, 4) != checkpointMagic ||
        readUnsigned(bytes, 4, 4) != checksum(bytes)) fail("STORAGE_CORRUPTION: checkpoint record");
        // 读满、魔数、校验和都要正确。
    checkpointRecord_.present = true;
    // 标记记录存在。
    checkpointRecord_.walCutoffBytes = readUnsigned(bytes, 16, 8);
    // 读日志截止位置。
    checkpointRecord_.dirtyWatermark = readUnsigned(bytes, 24, 8);
    // 读脏页水位。
    checkpointRecord_.catalogVersion = readUnsigned(bytes, 32, 8);
    // 读目录版本。
    checkpointRecord_.indexVersion = readUnsigned(bytes, 40, 8);
    // 读索引版本。
    checkpointRecord_.committedSequence = readUnsigned(bytes, 48, 8);
    // 读提交序号。
    checkpointRecord_.timestampMs = readUnsigned(bytes, 56, 8);
    // 读时间戳。
    checkpointRecord_.walLsn = readUnsigned(bytes, cLastLsn, 8);
    checkpointRecord_.checkpointBeginLsn = readUnsigned(bytes, cBeginLsn, 8);
    checkpointRecord_.checkpointEndLsn = readUnsigned(bytes, cEndLsn, 8);
    checkpointRecord_.archivedBytes = readUnsigned(bytes, cArchivedBytes, 8);
    checkpointRecord_.archiveSegments = readUnsigned(bytes, cArchiveSegments, 8);
    dirtyWatermark_ = checkpointRecord_.dirtyWatermark;
    // 同步内存水位。
    committedSequence_ = checkpointRecord_.committedSequence;
    // 同步内存提交序号，保证后续提交序号从检查点继续递增。
    nextLsn_ = checkpointRecord_.walLsn > 0 ? checkpointRecord_.walLsn + 1 : 1;
    // 日志截断后 prevLsn 链尾也要恢复，否则截断后的第一条扩展会断开链。
    lastExtentLsn_ = checkpointRecord_.walLsn;
}
}
