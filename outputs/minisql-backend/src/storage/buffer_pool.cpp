#include "minisql/storage/buffer_pool.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <fstream>
#include <utility>

namespace minisql::storage {
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
void validPolicy(ReplacementPolicy policy) {
    if (policy != ReplacementPolicy::LRU && policy != ReplacementPolicy::FIFO) fail("Unknown replacement policy");
}
}
PageGuard::PageGuard(std::shared_ptr<BufferFrame> frame) : frame_(std::move(frame)) { ++frame_->pins; }
PageGuard::~PageGuard() { if (frame_) --frame_->pins; }
PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
    if (this != &other) {
        if (frame_) --frame_->pins;
        frame_ = std::move(other.frame_);
    }
    return *this;
}
const SlottedPage& PageGuard::page() const {
    if (!frame_) fail("Moved-from page guard");
    return frame_->page;
}
SlotRef PageGuard::insert(std::span<const std::uint8_t> record) {
    if (!frame_) fail("Moved-from page guard");
    auto ref = frame_->page.insert(record);
    frame_->dirty = true;
    return ref;
}
void PageGuard::erase(SlotRef ref) {
    if (!frame_) fail("Moved-from page guard");
    frame_->page.erase(ref);
    frame_->dirty = true;
}
BufferPool::BufferPool(std::shared_ptr<PageFile> file, std::size_t capacity, ReplacementPolicy policy)
    : file_(std::move(file)), capacity_(capacity), policy_(policy) {
    if (!file_ || capacity == 0) fail("Buffer requires a file and positive capacity");
    validPolicy(policy);
}
void BufferPool::writeBack(BufferFrame& frame) {
    if (!frame.dirty) return;
    try {
        file_->write(frame.page);
        file_->flush();
    } catch (...) {
        ++stats_.ioErrors;
        throw;
    }
    if (file_->writeBatchActive()) ++stats_.stagedPageWrites;
    else ++stats_.pageWrites;
    frame.dirty = false;
}
void BufferPool::setEvictionLog(std::filesystem::path path) {
    evictionLogPath_ = std::move(path);
}
void BufferPool::appendEvictionLog(const Eviction& event) const {
    if (evictionLogPath_.empty()) return;
    std::ofstream stream(evictionLogPath_, std::ios::app);
    // 日志写入失败不改变替换语义：统计与淘汰仍以内存状态为准。
    if (!stream) return;
    const auto atMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    stream << atMs << " seq=" << event.sequence
           << " policy=" << (event.policy == ReplacementPolicy::LRU ? "LRU" : "FIFO")
           << " page=" << event.page.id
           << " generation=" << event.page.generation
           << " dirty=" << (event.dirty ? 1 : 0)
           << " writeBack=" << event.writeBack << '\n';
}
void BufferPool::makeRoom() {
    if (frames_.size() < capacity_) return;
    auto victim = frames_.end();
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (it->second->pins) continue;
        const auto age = policy_ == ReplacementPolicy::LRU ? it->second->accessed : it->second->loaded;
        if (victim == frames_.end() || age < (policy_ == ReplacementPolicy::LRU ? victim->second->accessed : victim->second->loaded)) victim = it;
    }
    if (victim == frames_.end()) fail("BUFFER_EXHAUSTED: all frames pinned");
    const auto& frame = *victim->second;
    Eviction event{sequence_ + 1, policy_, {frame.page.id(), frame.page.generation()}, frame.dirty};
    try { writeBack(*victim->second); }
    catch (...) {
        event.writeBack = "failed";
        evictions_.push_back(event);
        appendEvictionLog(event);
        throw;
    }
    if (event.dirty) event.writeBack = file_->writeBatchActive() ? "staged" : "written";
    evictions_.push_back(event);
    appendEvictionLog(event);
    frames_.erase(victim);
}
PageGuard BufferPool::get(PageRef ref) {
    file_->requireHealthy();
    auto found = frames_.find(ref.id);
    if (found != frames_.end()) {
        if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
        found->second->accessed = ++sequence_;
        ++stats_.hits;
        return PageGuard(found->second);
    }
    makeRoom();
    auto page = readPage(ref);
    auto frame = std::make_shared<BufferFrame>(BufferFrame{std::move(page), 0, false, ++sequence_, sequence_});
    frames_.emplace(ref.id, frame);
    ++stats_.misses;
    return PageGuard(std::move(frame));
}
PageRef BufferPool::allocate(std::uint64_t owner) {
    makeRoom();
    return file_->allocate(owner);
}
void BufferPool::release(PageRef ref) {
    auto found = frames_.find(ref.id);
    if (found != frames_.end()) {
        if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
        if (found->second->pins) fail("PAGE_PINNED");
    }
    file_->release(ref);
    if (found != frames_.end()) frames_.erase(found);
}
void BufferPool::flush(PageRef ref) {
    auto found = frames_.find(ref.id);
    if (found == frames_.end()) {
        (void)readPage(ref);
        return;
    }
    if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
    if (found->second->pins) fail("PAGE_PINNED");
    writeBack(*found->second);
}
void BufferPool::requireUnpinned() const {
    for (const auto& [id, frame] : frames_) if (frame->pins) fail("PAGE_PINNED");
}
SlottedPage BufferPool::readPage(PageRef ref) {
    try {
        auto page = file_->read(ref);
        if (file_->hasStagedPage(ref.id)) ++stats_.stagedPageReads;
        else ++stats_.pageReads;
        return page;
    } catch (...) {
        ++stats_.ioErrors;
        throw;
    }
}
void BufferPool::flushAll() {
    requireUnpinned();
    for (auto& [id, frame] : frames_) writeBack(*frame);
    file_->flush();
}
void BufferPool::beginWriteBatch() {
    if (file_->writeBatchActive()) throw MiniSqlError(ErrorCode::Transaction, "Nested write batches are not supported");
    flushAll();
    file_->beginWriteBatch();
}
void BufferPool::rollbackWriteBatch() {
    requireUnpinned();
    file_->rollbackWriteBatch();
    // 回滚后不能把仍在缓存中的候选页再次写回。
    frames_.clear();
}
void BufferPool::restoreSavepoint(const PageFileSavepoint& snapshot) {
    requireUnpinned();
    file_->restoreSavepoint(snapshot);
    frames_.clear();
}
void BufferPool::commitWriteBatch() {
    if (!file_->writeBatchActive()) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    flushAll();
    file_->commitWriteBatch();
}
void BufferPool::setPolicy(ReplacementPolicy policy) {
    validPolicy(policy);
    flushAll();
    frames_.clear();
    policy_ = policy;
    resetStats();
}
void BufferPool::resetStats() {
    stats_ = {};
    file_->resetIoStats();
    evictions_.clear();
}
std::size_t BufferPool::pins(PageRef ref) const {
    auto found = frames_.find(ref.id);
    if (found == frames_.end()) return 0;
    if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
    return found->second->pins;
}
std::size_t BufferPool::dirtyPages() const {
    std::size_t result = 0;
    for (const auto& [id, frame] : frames_) if (frame->dirty) ++result;
    return result;
}
}
