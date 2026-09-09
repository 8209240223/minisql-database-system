#include "minisql/storage/page.hpp"
#include "minisql/common/error.hpp"
#include <algorithm>
#include <limits>

namespace minisql::storage {
namespace {
constexpr std::size_t headerSize = 64;
constexpr std::size_t slotSize = 16;
constexpr std::uint32_t magic = 0x5047534d;
std::size_t slotOffset(std::uint32_t slot) { return headerSize + slot * slotSize; }
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
}

std::uint64_t readUnsigned(const PageBytes& bytes, std::size_t offset, std::size_t width) {
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset) fail("Invalid page field bounds");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t(bytes[offset + i]) << (8 * i);
    return value;
}
void writeUnsigned(PageBytes& bytes, std::size_t offset, std::size_t width, std::uint64_t value) {
    if (width > 8 || offset > bytes.size() || width > bytes.size() - offset) fail("Invalid page field bounds");
    if (width < 8 && (value >> (width * 8)) != 0) fail("Page field overflow");
    for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t checksum(const PageBytes& bytes) {
    std::uint32_t value = 2166136261u;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i >= 4 && i < 8) continue;
        value = (value ^ bytes[i]) * 16777619u;
    }
    return value;
}

SlottedPage::SlottedPage(PageId id, std::uint64_t owner, std::uint64_t generation) {
    if (id == kInvalidPageId || generation == 0) fail("Invalid page identity");
    writeUnsigned(bytes_, 0, 4, magic);
    writeUnsigned(bytes_, 8, 8, id);
    writeUnsigned(bytes_, 16, 8, owner);
    writeUnsigned(bytes_, 24, 8, generation);
    writeUnsigned(bytes_, 32, 4, 1);
    writeUnsigned(bytes_, 40, 4, kPageSize);
}
SlottedPage::SlottedPage(PageBytes bytes) : bytes_(std::move(bytes)) {
    if (readUnsigned(bytes_, 4, 4) != checksum(bytes_)) fail("STORAGE_CORRUPTION: page checksum");
    validate();
}
PageId SlottedPage::id() const { return readUnsigned(bytes_, 8, 8); }
std::uint64_t SlottedPage::owner() const { return readUnsigned(bytes_, 16, 8); }
std::uint64_t SlottedPage::generation() const { return readUnsigned(bytes_, 24, 8); }

void SlottedPage::validate() const {
    if (readUnsigned(bytes_, 0, 4) != magic || readUnsigned(bytes_, 32, 4) != 1 || generation() == 0 || id() == kInvalidPageId)
        fail("STORAGE_CORRUPTION: page header");
    const auto count = readUnsigned(bytes_, 36, 4);
    const auto lower = readUnsigned(bytes_, 40, 4);
    if (count > (kPageSize - headerSize) / slotSize || lower < headerSize + count * slotSize || lower > kPageSize)
        fail("STORAGE_CORRUPTION: slot directory bounds");
    std::vector<std::pair<std::size_t, std::size_t>> intervals;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = slotOffset(i);
        const auto offset = readUnsigned(bytes_, entry, 4);
        const auto length = readUnsigned(bytes_, entry + 4, 4);
        if (readUnsigned(bytes_, entry + 8, 8) == 0) fail("STORAGE_CORRUPTION: slot generation");
        if (length == 0) continue;
        if (offset < lower || offset > kPageSize || length > kPageSize - offset) fail("STORAGE_CORRUPTION: record bounds");
        intervals.emplace_back(offset, offset + length);
    }
    std::sort(intervals.begin(), intervals.end());
    for (std::size_t i = 1; i < intervals.size(); ++i)
        if (intervals[i].first < intervals[i - 1].second) fail("STORAGE_CORRUPTION: overlapping records");
}

void SlottedPage::compact() {
    const auto old = bytes_;
    const auto count = readUnsigned(bytes_, 36, 4);
    const auto directoryEnd = headerSize + count * slotSize;
    std::fill(bytes_.begin() + directoryEnd, bytes_.end(), 0);
    std::size_t end = kPageSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = slotOffset(i);
        const auto length = readUnsigned(old, entry + 4, 4);
        if (length == 0) continue;
        const auto offset = readUnsigned(old, entry, 4);
        end -= length;
        std::copy_n(old.begin() + offset, length, bytes_.begin() + end);
        writeUnsigned(bytes_, entry, 4, end);
    }
    writeUnsigned(bytes_, 40, 4, end);
}

std::size_t SlottedPage::freeSpace() const {
    const auto count = readUnsigned(bytes_, 36, 4);
    std::size_t used = headerSize + count * slotSize;
    for (std::uint32_t i = 0; i < count; ++i) used += readUnsigned(bytes_, slotOffset(i) + 4, 4);
    return kPageSize - used;
}

SlotRef SlottedPage::insert(std::span<const std::uint8_t> record) {
    if (record.empty()) fail("Empty physical record is not supported");
    if (record.size() > kPageSize - headerSize - slotSize) fail("ROW_TOO_LARGE");
    const auto count = static_cast<std::uint32_t>(readUnsigned(bytes_, 36, 4));
    auto slot = count;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4) == 0 &&
            readUnsigned(bytes_, slotOffset(i) + 8, 8) != std::numeric_limits<std::uint64_t>::max()) { slot = i; break; }
    }
    if (record.size() + (slot == count ? slotSize : 0) > freeSpace()) fail("PAGE_FULL");
    compact();
    const auto entry = slotOffset(slot);
    const auto generation = slot == count ? 1 : readUnsigned(bytes_, entry + 8, 8) + 1;
    const auto offset = readUnsigned(bytes_, 40, 4) - record.size();
    std::copy(record.begin(), record.end(), bytes_.begin() + offset);
    writeUnsigned(bytes_, entry, 4, offset);
    writeUnsigned(bytes_, entry + 4, 4, record.size());
    writeUnsigned(bytes_, entry + 8, 8, generation);
    writeUnsigned(bytes_, 40, 4, offset);
    if (slot == count) writeUnsigned(bytes_, 36, 4, count + 1);
    return {slot, generation};
}
bool SlottedPage::canInsert(std::size_t bytes) const {
    if (bytes == 0 || bytes > kPageSize - headerSize - slotSize) return false;
    const auto count = readUnsigned(bytes_, 36, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4) == 0 &&
            readUnsigned(bytes_, slotOffset(i) + 8, 8) != std::numeric_limits<std::uint64_t>::max()) return bytes <= freeSpace();
    }
    return bytes + slotSize <= freeSpace();
}
void SlottedPage::requireLive(SlotRef ref) const {
    if (ref.slot >= readUnsigned(bytes_, 36, 4)) fail("STALE_ROW_ID");
    const auto entry = slotOffset(ref.slot);
    if (readUnsigned(bytes_, entry + 4, 4) == 0 || readUnsigned(bytes_, entry + 8, 8) != ref.generation) fail("STALE_ROW_ID");
}
std::vector<std::uint8_t> SlottedPage::read(SlotRef ref) const {
    requireLive(ref);
    const auto entry = slotOffset(ref.slot);
    const auto offset = readUnsigned(bytes_, entry, 4);
    const auto length = readUnsigned(bytes_, entry + 4, 4);
    return {bytes_.begin() + offset, bytes_.begin() + offset + length};
}
void SlottedPage::erase(SlotRef ref) {
    requireLive(ref);
    const auto entry = slotOffset(ref.slot);
    const auto offset = readUnsigned(bytes_, entry, 4);
    const auto length = readUnsigned(bytes_, entry + 4, 4);
    std::fill_n(bytes_.begin() + offset, length, 0);
    writeUnsigned(bytes_, entry, 4, 0);
    writeUnsigned(bytes_, entry + 4, 4, 0);
}
std::vector<SlotRef> SlottedPage::liveSlots() const {
    std::vector<SlotRef> result;
    for (std::uint32_t i = 0; i < readUnsigned(bytes_, 36, 4); ++i)
        if (readUnsigned(bytes_, slotOffset(i) + 4, 4)) result.push_back({i, readUnsigned(bytes_, slotOffset(i) + 8, 8)});
    return result;
}
PageBytes SlottedPage::serialize() const {
    validate();
    auto result = bytes_;
    writeUnsigned(result, 4, 4, checksum(result));
    return result;
}
}
