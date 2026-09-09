#pragma once
#include "minisql/common/types.hpp"
#include <array>
#include <span>
#include <vector>

namespace minisql::storage {
inline constexpr std::size_t kPageSize = 4096;
using PageBytes = std::array<std::uint8_t, kPageSize>;
struct SlotRef {
    std::uint32_t slot;
    std::uint64_t generation;
};

class SlottedPage {
public:
    SlottedPage(PageId id, std::uint64_t owner, std::uint64_t generation = 1);
    explicit SlottedPage(PageBytes bytes);
    PageId id() const;
    std::uint64_t owner() const;
    std::uint64_t generation() const;
    SlotRef insert(std::span<const std::uint8_t> record);
    std::vector<std::uint8_t> read(SlotRef reference) const;
    void erase(SlotRef reference);
    std::vector<SlotRef> liveSlots() const;
    std::size_t freeSpace() const;
    bool canInsert(std::size_t bytes) const;
    PageBytes serialize() const;
private:
    PageBytes bytes_{};
    void validate() const;
    void requireLive(SlotRef reference) const;
    void compact();
};

std::uint64_t readUnsigned(const PageBytes& bytes, std::size_t offset, std::size_t width);
void writeUnsigned(PageBytes& bytes, std::size_t offset, std::size_t width, std::uint64_t value);
std::uint32_t checksum(const PageBytes& bytes);
}
