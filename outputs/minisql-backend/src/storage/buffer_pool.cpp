#include "minisql/storage/buffer_pool.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <fstream>
#include <utility>

namespace minisql::storage {
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
// 统一的存储错误出口。
void validPolicy(ReplacementPolicy policy) {
// 校验策略取值合法。
    if (policy != ReplacementPolicy::LRU && policy != ReplacementPolicy::FIFO && policy != ReplacementPolicy::CLOCK) fail("Unknown replacement policy");
    // 目前只支持两种策略，其他取值一律拒绝。
}
}
PageGuard::PageGuard(std::shared_ptr<BufferFrame> frame) : frame_(std::move(frame)) { ++frame_->pins; }
// 构造凭证时把页的 pin 计数加一，表示“有人正在用这一页”。
PageGuard::~PageGuard() { if (frame_) --frame_->pins; }
// 析构时减一；减到零之后这一页才允许被淘汰。
PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
// 移动赋值：接手新页之前要先松开自己原来持有的页。
    if (this != &other) {
    // 防止自赋值。
        if (frame_) --frame_->pins;
        // 释放原有引用的 pin。
        frame_ = std::move(other.frame_);
        // 接管对方的帧；对方的 shared_ptr 变为空。
    }
    return *this;
    // 返回自身以支持链式赋值。
}
const SlottedPage& PageGuard::page() const {
// 取页内容。
    if (!frame_) fail("Moved-from page guard");
    // 被移动走的凭证已经没有有效帧，使用它属于调用方错误。
    return frame_->page;
    // 返回帧里的页。
}
SlotRef PageGuard::insert(std::span<const std::uint8_t> record) {
// 通过凭证往页里插入记录。
    if (!frame_) fail("Moved-from page guard");
    // 同样先检查凭证是否还有效。
    auto ref = frame_->page.insert(record);
    // 交给页自己插入，拿到槽引用。
    frame_->dirty = true;
    // 页被修改，标记为脏页，淘汰前必须写回。
    return ref;
    // 返回槽引用。
}
void PageGuard::erase(SlotRef ref) {
// 通过凭证删除页内记录。
    if (!frame_) fail("Moved-from page guard");
    // 检查凭证有效性。
    frame_->page.erase(ref);
    // 删除记录，槽号仍保留在目录里以便复用。
    frame_->dirty = true;
    // 同样标记脏页。
}
BufferPool::BufferPool(std::shared_ptr<PageFile> file, std::size_t capacity, ReplacementPolicy policy)
    : file_(std::move(file)), capacity_(capacity), policy_(policy) {
    // 保存页文件、容量与策略。
    if (!file_ || capacity == 0) fail("Buffer requires a file and positive capacity");
    // 没有页文件或者容量为零都构造不出有意义的缓冲池。
    validPolicy(policy);
    // 校验策略。
}
void BufferPool::writeBack(BufferFrame& frame) {
// 把一帧写回磁盘；只有脏页才需要真正写。
    if (!frame.dirty) return;
    // 干净页直接返回，省掉一次磁盘写。
    try {
        file_->write(frame.page);
        // 写入页文件。
        file_->flush();
        // 立即刷到磁盘，保证写回结果不丢。
    } catch (...) {
        ++stats_.ioErrors;
        // 记录一次 IO 错误。
        throw;
        // 继续往上抛，让调用方知道本次写回失败。
    }
    if (file_->writeBatchActive()) ++stats_.stagedPageWrites;
    // 处于写批次时，这次写只是进入暂存区，单独计数。
    else ++stats_.pageWrites;
    // 否则算一次真正落盘。
    frame.dirty = false;
    // 写回成功后才能清掉脏标记。
}
void BufferPool::setEvictionLog(std::filesystem::path path) {
// 设置替换日志路径。
    evictionLogPath_ = std::move(path);
    // 空路径表示关闭日志。
}
void BufferPool::appendEvictionLog(const Eviction& event) const {
// 把一条淘汰事件追加到日志文件。
    if (evictionLogPath_.empty()) return;
    // 没有配置日志就什么也不做。
    std::ofstream stream(evictionLogPath_, std::ios::app);
    // 以追加方式打开，保证历史记录不被覆盖。
    // 日志写入失败不改变替换语义：统计与淘汰仍以内存状态为准。
    if (!stream) return;
    // 打不开文件就静默跳过，不能因为写日志失败而影响数据库。
    const auto atMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // 取当前时间的毫秒表示。
    stream << atMs << " seq=" << event.sequence
           << " policy=" << (event.policy == ReplacementPolicy::LRU ? "LRU" : event.policy == ReplacementPolicy::CLOCK ? "CLOCK" : "FIFO")
           << " page=" << event.page.id
           << " generation=" << event.page.generation
           << " dirty=" << (event.dirty ? 1 : 0)
           << " writeBack=" << event.writeBack << '\n';
    // 输出一行结构化文本：时间、序号、策略、页号、代数、脏页标志、写回结果。
}
PageId BufferPool::clockEvict() {
// CLOCK（二次机会）选 victim：环形扫描，参考位为 1 的给一次机会并清位。
// 与 LRU 的核心差异：LRU 要维护全局访问序号并每次都换入淼求最旧页，
// CLOCK 只用一个引用位加一个环形指针，每次选 victim 的平均扫描步数远小于遍历全表。
    if (clockOrder_.empty()) return {};
    // 环形顺序为空说明一页未驻留，无法选择。
    for (;;) {
    // 一直扫到找到可淘汰的页为止。
        if (clockOrder_.empty()) return {};
        // 每轮开始都再确认环里还有页。
        clockHand_ %= clockOrder_.size();
        // 指针转到环内（避免因淘汰导致越界）。
        const auto candidate = clockOrder_[clockHand_];
        // 取当前指针指向的页号。
        auto found = frames_.find(candidate);
        // 到缓存里取该页。
        if (found == frames_.end()) {
        // 页已不在缓存（可能被 release 掉），从环里剔除。
            clockOrder_.erase(clockOrder_.begin() + static_cast<std::ptrdiff_t>(clockHand_));
            continue;
            // 继续扫描，指针不动（删除后同一位置已换成后继）。
        }
        // 找到对应帧。
        ++stats_.clockSweeps;
        ++stats_.evictionScans;
        // CLOCK 的一次扫描同样计入 evictionScans，便于与 LRU 同口径对比。
        // 记一次扫描步数，用于量化算法成本。
        auto& frame = *found->second;
        // 取帧引用。
        if (frame.pins) {
        // 被 pin 住的页不能淘汰：指针前移，继续找。
            clockHand_ = (clockHand_ + 1) % clockOrder_.size();
            continue;
        }
        // pin 检查结束。
        if (frame.referenced) {
        // 引用位为 1：这页最近被用过，给它一次机会。
            frame.referenced = false;
            // 清位，下一圈再扫到它就会被淘汰。
            ++stats_.clockSecondChances;
            // 记一次二次机会，便于对比 LRU。
            clockHand_ = (clockHand_ + 1) % clockOrder_.size();
            // 指针前移继续扫。
            continue;
            // 本轮不淘汰它。
        }
        // 引用位为 0：这页就是 victim。
        clockOrder_.erase(clockOrder_.begin() + static_cast<std::ptrdiff_t>(clockHand_));
        // 从环里移除（指针保持不动，自然指向后继）。
        return candidate;
        // 返回被选中的页号。
    }
    // 不可达（循环内总会 return）。
}
void BufferPool::makeRoom() {
// 缓存未满直接返回；满了就挑一页淘汰，必要时先写回。
    if (frames_.size() < capacity_) return;
    // 还有空位，不需要淘汰。
    if (policy_ == ReplacementPolicy::CLOCK) {
    // CLOCK：用环形扫描选 victim，不走下面按序号遍历的 LRU/FIFO 路径。
        const auto chosen = clockEvict();
        // 环形扫描得到要淘汰的页号。
        const auto target = frames_.find(chosen);
        // 再到缓存里取它。
        if (target == frames_.end()) fail("CLOCK selected a page that is not resident");
        // 选出的页必须仍在缓存里，否则说明环形结构与缓存不一致。
        const auto& frame = *target->second;
        // 取该帧。
        Eviction event{sequence_ + 1, policy_, {frame.page.id(), frame.page.generation()}, frame.dirty};
        // 先构造淘汰事件。
        try { writeBack(*target->second); }
        // 脏页先写回。
        catch (...) {
            event.writeBack = "failed";
            // 写回失败要在记录里体现。
            evictions_.push_back(event);
            appendEvictionLog(event);
            throw;
            // 写回失败不能继续淘汰，否则会丢数据。
        }
        // 写回分支结束。
        if (event.dirty) event.writeBack = file_->writeBatchActive() ? "staged" : "written";
        // 记录脏页的实际去向。
        evictions_.push_back(event);
        appendEvictionLog(event);
        // 登记淘汰事件。
        frames_.erase(target);
        // 从缓存移除；环形顺序已在 clockEvict 里同步移除。
        return;
        // CLOCK 分支结束。
    }
    // 非 CLOCK 策略继续走按序号挑选的路径。
    auto victim = frames_.end();
    // 候选受害者，初始为空。
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    ++stats_.evictionScans;
    // 记一次"检查了一个帧"，用于量化淘汰搜索代价。
    // 遍历所有缓存帧。
        if (it->second->pins) continue;
        // 正在被使用的页不能淘汰。
        const auto age = policy_ == ReplacementPolicy::LRU ? it->second->accessed : it->second->loaded;
        // LRU 看最近访问序号的页，FIFO 看装入序号。
        if (victim == frames_.end() || age < (policy_ == ReplacementPolicy::LRU ? victim->second->accessed : victim->second->loaded)) victim = it;
        // 序号更小代表更久没用或更早装入，作为更优的淘汰对象。
    }
    if (victim == frames_.end()) fail("BUFFER_EXHAUSTED: all frames pinned");
    // 所有页都被占用时无法腾空间，只能报错。
    const auto& frame = *victim->second;
    // 取出被选中的帧。
    Eviction event{sequence_ + 1, policy_, {frame.page.id(), frame.page.generation()}, frame.dirty};
    // 先构造一条淘汰事件记录。
    try { writeBack(*victim->second); }
    // 尝试把脏页写回。
    catch (...) {
        event.writeBack = "failed";
        // 写回失败要在记录里体现。
        evictions_.push_back(event);
        // 保留这条失败记录。
        appendEvictionLog(event);
        // 同时写进文本日志。
        throw;
        // 写回失败不能继续淘汰，否则会丢数据。
    }
    if (event.dirty) event.writeBack = file_->writeBatchActive() ? "staged" : "written";
    // 脏页写回成功，记录实际去向是暂存区还是磁盘。
    evictions_.push_back(event);
    // 登记成功记录。
    appendEvictionLog(event);
    // 追加日志。
    frames_.erase(victim);
    // 从缓存表移除，这个位置就空出来给新页用。
}
PageGuard BufferPool::get(PageRef ref) {
// 取页：先查缓存，命中就直接用，否则腾空间后从磁盘读入。
    file_->requireHealthy();
    // 页文件处于故障状态时禁止继续访问。
    auto found = frames_.find(ref.id);
    // 用页号在映射表里查找。
    if (found != frames_.end()) {
    // 命中分支。
        if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
        // 代数不一致说明引用来自被回收前的旧页。
        found->second->accessed = ++sequence_;
        found->second->referenced = true;
        // CLOCK 的引用位：命中即置 1，表示这一页最近被访问过，淘汰时值得再给一次机会。
        // 更新最近访问序号，供 LRU 使用。
        ++stats_.hits;
        // 命中计数加一。
        return PageGuard(found->second);
        // 返回凭证，pin 加一。
    }
    makeRoom();
    // 未命中，先确保有位置。
    auto page = readPage(ref);
    // 从页文件读入这一页。
    auto frame = std::make_shared<BufferFrame>(BufferFrame{std::move(page), 0, false, ++sequence_, sequence_});
    // 新建缓存帧：pin 为 0、非脏，装入与访问序号都取当前递增值。
    frames_.emplace(ref.id, frame);
    frame->referenced = true;
    // 新装入的页刚被访问过，引用位置 1。
    if (policy_ == ReplacementPolicy::CLOCK) clockOrder_.push_back(ref.id);
    // CLOCK 需要维护环形顺序：新页追加到环尾。
    // 放入映射表。
    ++stats_.misses;
    // 未命中计数加一。
    return PageGuard(std::move(frame));
    // 返回凭证。
}
PageRef BufferPool::allocate(std::uint64_t owner) {
// 分配一张新页给指定的归属者。
    makeRoom();
    // 先腾出缓存空间。
    return file_->allocate(owner);
    // 真正的分配交给页文件，它负责空闲页列表与页号分配。
}
void BufferPool::release(PageRef ref) {
// 释放一张页。
    auto found = frames_.find(ref.id);
    // 先看它在不在缓存里。
    if (found != frames_.end()) {
    // 在缓存里就要做两项检查。
        if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
        // 代数校验。
        if (found->second->pins) fail("PAGE_PINNED");
        // 有人正在使用这一页时不允许释放。
    }
    file_->release(ref);
    // 交给页文件回收，并写入释放标记。
    if (found != frames_.end()) frames_.erase(found);
    // 从缓存中移除。
}
void BufferPool::flush(PageRef ref) {
// 把指定页写回磁盘。
    auto found = frames_.find(ref.id);
    // 查缓存。
    if (found == frames_.end()) {
    // 不在缓存里说明磁盘上的内容已经是最新的。
        (void)readPage(ref);
        // 顺带读一次以便统计与校验，(void) 表示不关心返回值。
        return;
        // 直接返回。
    }
    if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
    // 代数校验。
    if (found->second->pins) fail("PAGE_PINNED");
    // 使用中的页不允许刷新，避免写出一半的内容。
    writeBack(*found->second);
    // 写回并清脏标记。
}
void BufferPool::requireUnpinned() const {
// 整体操作前的安全检查：确认没有任何页被 pin。
    for (const auto& [id, frame] : frames_) if (frame->pins) fail("PAGE_PINNED");
    // 只要有一页被占用就报错。
}
SlottedPage BufferPool::readPage(PageRef ref) {
// 从页文件读取一页，并维护读写统计。
    try {
        auto page = file_->read(ref);
        // 真正读磁盘（或暂存区）。
        if (file_->hasStagedPage(ref.id)) ++stats_.stagedPageReads;
        // 命中的是写批次暂存内容，单独计数。
        else ++stats_.pageReads;
        // 否则算一次真正的磁盘读。
        return page;
        // 返回读到的页。
    } catch (...) {
        ++stats_.ioErrors;
        // 记录 IO 错误。
        throw;
        // 继续上抛。
    }
}
void BufferPool::flushAll() {
// 把所有脏页写回磁盘。
    requireUnpinned();
    // 有页被占用时不允许整体刷盘。
    for (auto& [id, frame] : frames_) writeBack(*frame);
    // 逐帧写回，干净页会自动跳过。
    file_->flush();
    // 最后统一刷一次文件缓冲。
}
void BufferPool::beginWriteBatch() {
// 开始写批次：进入“改动只进暂存区、可整体回滚”的状态。
    if (file_->writeBatchActive()) throw MiniSqlError(ErrorCode::Transaction, "Nested write batches are not supported");
    // 不允许嵌套写批次。
    flushAll();
    // 先把之前的脏页清干净，批次内只处理新改动。
    file_->beginWriteBatch();
    // 让页文件进入批次模式并建立回滚点。
}
void BufferPool::rollbackWriteBatch() {
// 回滚写批次：撤销暂存改动并丢弃缓存。
    requireUnpinned();
    // 有页正在使用时不安全。
    file_->rollbackWriteBatch();
    // 让页文件恢复到批次开始时的状态。
    // 回滚后不能把仍在缓存中的候选页再次写回。
    frames_.clear();
    // 清空缓存，避免保留了已撤销内容的页在之后被淘汰时写回磁盘。
}
void BufferPool::restoreSavepoint(const PageFileSavepoint& snapshot) {
    requireUnpinned();
    file_->restoreSavepoint(snapshot);
    frames_.clear();
}
void BufferPool::commitWriteBatch() {
// 提交写批次：把改动正式落盘。
    if (!file_->writeBatchActive()) throw MiniSqlError(ErrorCode::Transaction, "No active write batch");
    // 当前没有批次却要提交，属于调用错误。
    flushAll();
    // 先确保缓存里的改动都进了暂存区。
    file_->commitWriteBatch();
    // 由页文件写入提交标记，使改动正式生效。
}
void BufferPool::setPolicy(ReplacementPolicy policy) {
// 切换淘汰策略。
    validPolicy(policy);
    // 校验取值。
    flushAll();
    // 切换前先刷盘，避免丢失脏页。
    frames_.clear();
    // 清空缓存，让新策略从干净状态开始。
    policy_ = policy;
    // 记录新策略。
    resetStats();
    // 统计清零，便于对比两种策略的命中率。
}
void BufferPool::resetStats() {
// 清空统计与淘汰记录。
    stats_ = {};
    clockHand_ = 0;
    // CLOCK 指针归零，重新从环首开始扫描。
    clockOrder_.clear();
    // 先清空再按当前实际驻留的页重建：
    // RESET 动作只清统计、不清缓存，若只清不建，
    // clockEvict 就看不到已驻留的页，选 victim 时会与缓存状态不一致。
    for (const auto& entry : frames_) clockOrder_.push_back(entry.first);
    // 按当前驻留的页重建环形顺序（顺序不敏感，CLOCK 只需环形）。
    // 统计结构恢复默认值。
    file_->resetIoStats();
    // 页文件层的读写计数也清零。
    evictions_.clear();
    // 淘汰历史清空。
}
std::size_t BufferPool::pins(PageRef ref) const {
// 查询某页当前被 pin 的次数。
    auto found = frames_.find(ref.id);
    // 查缓存。
    if (found == frames_.end()) return 0;
    // 不在缓存里就是 0。
    if (found->second->page.generation() != ref.generation) fail("STALE_PAGE_ID");
    // 代数不符说明引用过期。
    return found->second->pins;
    // 返回计数。
}
std::size_t BufferPool::dirtyPages() const {
// 统计当前脏页数量。
    std::size_t result = 0;
    // 计数器。
    for (const auto& [id, frame] : frames_) if (frame->dirty) ++result;
    // 遍历缓存，脏页就加一。
    return result;
}
}
