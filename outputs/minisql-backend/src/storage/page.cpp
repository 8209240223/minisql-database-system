#include "minisql/storage/page.hpp"
#include "minisql/common/error.hpp"
#include <algorithm>
#include <limits>

namespace minisql::storage {
namespace {
constexpr std::size_t headerSize = 64;
// 页头固定占前 64 字节。
constexpr std::size_t slotSize = 16;
// 每个槽目录项固定占 16 字节。
constexpr std::uint32_t magic = 0x5047534d;
// 页魔数，用来快速判断这 4096 字节到底是不是一张合法的页。
std::size_t slotOffset(std::uint32_t slot) { return headerSize + slot * slotSize; }
// 由槽号算出该槽目录项在页内的起始偏移。
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
// 统一的存储错误出口，[[noreturn]] 表示必然抛出。
}

std::uint64_t readUnsigned(const PageBytes& bytes, std::size_t offset, std::size_t width) {
// 读取小端无符号整数，width 表示读几个字节。
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset) fail("Invalid page field bounds");
    // 先做边界检查：字段宽度不能超过 8 字节，读取范围不能越出这一页。
    std::uint64_t value = 0;
    // 累加结果。
    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t(bytes[offset + i]) << (8 * i);
    // 逐字节按小端序拼装：第 i 个字节放在第 8i 位上。
    return value;
    // 返回读到的整数。
}
void writeUnsigned(PageBytes& bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
// 按小端序把整数写入页中指定位置。
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset) fail("Invalid page field bounds");
    // 同样的边界检查。
    if (width < 8 && (value >> (width * 8)) != 0) fail("Page field overflow");
    // 目标宽度放不下这个数值时报错，避免高位被静默丢弃。
    for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    // 逐字节截取低 8 位写入。
}
std::uint32_t checksum(const PageBytes& bytes) {
// 计算页校验和，用于检测落盘内容是否损坏。
    std::uint32_t value = 2166136261u;
    // FNV-1a 的初始值。
    for (std::size_t i = 0; i < bytes.size(); ++i) {
    // 遍历整页字节。
        if (i >= 4 && i < 8) continue;
        // 跳过第 4 到 7 字节，那正是校验和字段自身，不能参与计算。
        value = (value ^ bytes[i]) * 16777619u;
        // FNV-1a 的异或再乘质数。
    }
    return value;
    // 返回 32 位校验和。
}

SlottedPage::SlottedPage(PageId id, std::uint64_t owner, std::uint64_t generation) {
// 新建空白页：写入页头各字段，页内还没有任何记录。
    if (id == kInvalidPageId || generation == 0) fail("Invalid page identity");
    // 页号不能是无效值，代数必须从 1 开始。
    writeUnsigned(bytes_, 0, 4, magic);
    // 偏移 0 写 4 字节魔数。
    writeUnsigned(bytes_, 8, 8, id);
    // 偏移 8 写页号；偏移 4 到 7 留给校验和。
    writeUnsigned(bytes_, 16, 8, owner);
    // 偏移 16 写归属者标识。
    writeUnsigned(bytes_, 24, 8, generation);
    // 偏移 24 写代数。
    writeUnsigned(bytes_, 32, 4, 1);
    // 偏移 32 写格式版本号，当前为 1。
    writeUnsigned(bytes_, 40, 4, kPageSize);
    // 偏移 40 写“空闲区起始偏移”，空页时等于页大小，表示整页都空。
}
SlottedPage::SlottedPage(PageBytes bytes) : bytes_(std::move(bytes)) {
// 从磁盘字节还原成页，因此必须先验校验和再校验结构。
    if (readUnsigned(bytes_, 4, 4) != checksum(bytes_)) fail("STORAGE_CORRUPTION: page checksum");
    // 校验和字段存放在偏移 4，与重新计算的结果不一致说明数据被破坏。
    validate();
    // 校验页头与槽目录是否自洽。
}
PageId SlottedPage::id() const { return readUnsigned(bytes_, 8, 8); }
// 读取页号。
std::uint64_t SlottedPage::owner() const { return readUnsigned(bytes_, 16, 8); }
// 读取归属者。
std::uint64_t SlottedPage::generation() const { return readUnsigned(bytes_, 24, 8); }
// 读取页代数。

void SlottedPage::validate() const {
// 完整性检查：页头字段、槽目录范围、以及各记录之间不能重叠。
    if (readUnsigned(bytes_, 0, 4) != magic || readUnsigned(bytes_, 32, 4) != 1 || generation() == 0 || id() == kInvalidPageId)
    // 魔数、格式版本、代数、页号任一不对都判为页头损坏。
        fail("STORAGE_CORRUPTION: page header");
    const auto count = readUnsigned(bytes_, 36, 4);
    // 偏移 36 记录当前槽的数量。
    const auto lower = readUnsigned(bytes_, 40, 4);
    // 偏移 40 是记录区起始位置，也叫空闲区下界。
    if (count > (kPageSize - headerSize) / slotSize || lower < headerSize + count * slotSize || lower > kPageSize)
    // 槽数不能超过页内能容纳的上限；记录区不能压到槽目录上，也不能超出页尾。
        fail("STORAGE_CORRUPTION: slot directory bounds");
    std::vector<std::pair<std::size_t, std::size_t>> intervals;
    // 收集每条记录的起止区间，稍后检查重叠。
    for (std::uint32_t i = 0; i < count; ++i) {
    // 逐个槽检查。
        const auto entry = slotOffset(i);
        // 该槽目录项的偏移。
        const auto offset = readUnsigned(bytes_, entry, 4);
        // 槽里记录的记录起始偏移。
        const auto length = readUnsigned(bytes_, entry + 4, 4);
        // 槽里记录的长度；为 0 表示该槽已删除。
        if (readUnsigned(bytes_, entry + 8, 8) == 0) fail("STORAGE_CORRUPTION: slot generation");
        // 代数不允许为 0。
        if (length == 0) continue;
        // 已删除的槽不参与区间检查。
        if (offset < lower || offset > kPageSize || length > kPageSize - offset) fail("STORAGE_CORRUPTION: record bounds");
        // 记录必须落在记录区内且不能越过页尾。
        intervals.emplace_back(offset, offset + length);
        // 记下区间。
    }
    std::sort(intervals.begin(), intervals.end());
    // 按起点排序，便于相邻比较。
    for (std::size_t i = 1; i < intervals.size(); ++i)
    // 检查相邻区间是否交叠。
        if (intervals[i].first < intervals[i - 1].second) fail("STORAGE_CORRUPTION: overlapping records");
        // 后一个的起点早于前一个的终点即视为重叠。
}

void SlottedPage::compact() {
// 整理页内空间：把仍有效的记录重新紧凑地排到页尾，消除碎片。
    const auto old = bytes_;
    // 先备份原内容，边读边写。
    const auto count = readUnsigned(bytes_, 36, 4);
    // 槽总数。
    const auto directoryEnd = headerSize + count * slotSize;
    // 槽目录的结束偏移。
    std::fill(bytes_.begin() + directoryEnd, bytes_.end(), 0);
    // 把目录之后的内容全部清零，准备重排。
    std::size_t end = kPageSize;
    // 新记录区从页尾开始向前生长。
    for (std::uint32_t i = 0; i < count; ++i) {
    // 按槽号顺序搬运记录。
        const auto entry = slotOffset(i);
        // 当前槽目录项位置。
        const auto length = readUnsigned(old, entry + 4, 4);
        // 从备份读取长度。
        if (length == 0) continue;
        // 已删除的槽跳过。
        const auto offset = readUnsigned(old, entry, 4);
        // 从备份读取原偏移。
        end -= length;
        // 记录区下界向前移动。
        std::copy_n(old.begin() + offset, length, bytes_.begin() + end);
        // 把记录搬到新位置。
        writeUnsigned(bytes_, entry, 4, end);
        // 更新槽里的偏移，使它指向新位置。
    }
    writeUnsigned(bytes_, 40, 4, end);
    // 写回新的记录区下界。
}

std::size_t SlottedPage::freeSpace() const {
// 计算空闲字节数，等于整页减去槽目录与记录占用的部分。
    const auto count = readUnsigned(bytes_, 36, 4);
    // 槽总数。
    std::size_t used = headerSize + count * slotSize;
    // 已用空间先算上页头与槽目录。
    for (std::uint32_t i = 0; i < count; ++i) used += readUnsigned(bytes_, slotOffset(i) + 4, 4);
    // 再加上每条有效记录的长度。
    return kPageSize - used;
    // 差值就是空闲空间。
}

SlotRef SlottedPage::insert(std::span<const std::uint8_t> record) {
// 插入记录：优先复用已删除的槽，空间不足时先整理再判断。
    if (record.empty()) fail("Empty physical record is not supported");
    // 空记录没有意义，直接拒绝。
    if (record.size() > kPageSize - headerSize - slotSize) fail("ROW_TOO_LARGE");
    // 单条记录本身就超过一页能容纳的上限。
    const auto count = static_cast<std::uint32_t>(readUnsigned(bytes_, 36, 4));
    // 当前槽数。
    auto slot = count;
    // 默认使用新槽，也就是追加在目录末尾。
    for (std::uint32_t i = 0; i < count; ++i) {
    // 先找可复用的空槽。
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4) == 0 &&
            readUnsigned(bytes_, slotOffset(i) + 8, 8) != std::numeric_limits<std::uint64_t>::max()) { slot = i; break; }
        // 长度为 0 表示已删除；代数不等于 uint64 最大值表示它还能被复用。
    }
    if (record.size() + (slot == count ? slotSize : 0) > freeSpace()) fail("PAGE_FULL");
    // 若用新槽还要额外占一个目录项，两者相加超过空闲空间就是页满。
    compact();
    // 先整理出连续空间，再写入。
    const auto entry = slotOffset(slot);
    // 目标槽目录项位置。
    const auto generation = slot == count ? 1 : readUnsigned(bytes_, entry + 8, 8) + 1;
    // 新槽代数从 1 开始；复用旧槽时递增，让旧引用立即失效。
    const auto offset = readUnsigned(bytes_, 40, 4) - record.size();
    // 记录区下界前移，得到写入位置。
    std::copy(record.begin(), record.end(), bytes_.begin() + offset);
    // 把记录字节复制进页。
    writeUnsigned(bytes_, entry, 4, offset);
    // 槽里写记录偏移。
    writeUnsigned(bytes_, entry + 4, 4, record.size());
    // 槽里写记录长度，非零表示该槽有效。
    writeUnsigned(bytes_, entry + 8, 8, generation);
    // 槽里写代数。
    writeUnsigned(bytes_, 40, 4, offset);
    // 更新记录区下界。
    if (slot == count) writeUnsigned(bytes_, 36, 4, count + 1);
    // 用的是新槽就把槽数加一。
    return {slot, generation};
    // 返回槽引用，调用方可据此长期定位这条记录。
}
bool SlottedPage::canInsert(std::size_t bytes) const {
// 预判：这条记录能否放进去，逻辑与 insert 的空间判断保持一致。
    if (bytes == 0 || bytes > kPageSize - headerSize - slotSize) return false;
    // 空记录或超大记录直接不行。
    const auto count = readUnsigned(bytes_, 36, 4);
    // 槽数。
    for (std::uint32_t i = 0; i < count; ++i) {
    // 看看是否存在可复用的空槽。
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4) == 0 &&
            readUnsigned(bytes_, slotOffset(i) + 8, 8) != std::numeric_limits<std::uint64_t>::max()) return bytes <= freeSpace();
        // 有空槽就不必新增目录项，只比较记录长度。
    }
    return bytes + slotSize <= freeSpace();
    // 没有空槽时要为目录项多留 16 字节。
}
void SlottedPage::requireLive(SlotRef ref) const {
// 校验槽引用是否仍然指向一条有效记录。
    if (ref.slot >= readUnsigned(bytes_, 36, 4)) fail("STALE_ROW_ID");
    // 槽号超过当前槽数说明引用来自更早的版本。
    const auto entry = slotOffset(ref.slot);
    // 槽目录项位置。
    if (readUnsigned(bytes_, entry + 4, 4) == 0 || readUnsigned(bytes_, entry + 8, 8) != ref.generation) fail("STALE_ROW_ID");
    // 长度为 0（已删除）或代数不匹配，都说明引用过期。
}
std::vector<std::uint8_t> SlottedPage::read(SlotRef ref) const {
// 读出一条记录的字节内容。
    requireLive(ref);
    // 先确认引用有效。
    const auto entry = slotOffset(ref.slot);
    // 槽目录项位置。
    const auto offset = readUnsigned(bytes_, entry, 4);
    // 记录起始偏移。
    const auto length = readUnsigned(bytes_, entry + 4, 4);
    // 记录长度。
    return {bytes_.begin() + offset, bytes_.begin() + offset + length};
    // 复制这段字节返回，调用方得到的是独立副本。
}
void SlottedPage::erase(SlotRef ref) {
// 删除一条记录：把内容清零，并把槽的长度置 0。
    requireLive(ref);
    // 先确认引用有效。
    const auto entry = slotOffset(ref.slot);
    // 槽目录项位置。
    const auto offset = readUnsigned(bytes_, entry, 4);
    // 记录起始偏移。
    const auto length = readUnsigned(bytes_, entry + 4, 4);
    // 记录长度。
    std::fill_n(bytes_.begin() + offset, length, 0);
    // 清零记录内容，避免残留数据被误读。
    writeUnsigned(bytes_, entry, 4, 0);
    // 偏移清零。
    writeUnsigned(bytes_, entry + 4, 4, 0);
    // 长度清零，这一步才真正把槽标记为已删除。
}
std::vector<SlotRef> SlottedPage::liveSlots() const {
// 列出所有有效槽，扫描时按这个列表逐条读记录。
    std::vector<SlotRef> result;
    // 结果集合。
    for (std::uint32_t i = 0; i < readUnsigned(bytes_, 36, 4); ++i)
    // 遍历槽目录。
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4)) result.push_back({i, readUnsigned(bytes_, slotOffset(i) + 8, 8)});
        // 长度非零即为有效槽，记录槽号与代数。
    return result;
    // 返回有效槽列表。
}
PageBytes SlottedPage::serialize() const {
// 序列化：得到可以写入磁盘的字节。
    validate();
    // 先校验自身结构，避免把损坏的页写出去。
    auto result = bytes_;
    // 复制一份，不改动对象内部状态。
    writeUnsigned(result, 4, 4, checksum(result));
    // 在副本上写入校验和字段。
    return result;
    // 返回可落盘的字节序列。
}
}
