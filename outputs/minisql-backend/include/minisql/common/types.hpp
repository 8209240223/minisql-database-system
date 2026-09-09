#pragma once
#include <cstdint>
#include <limits>

namespace minisql {
using PageId = std::uint64_t;
using TransactionId = std::uint64_t;
using Lsn = std::uint64_t;
inline constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();
struct Rid {
    PageId pageId{kInvalidPageId};
    std::uint32_t slotId{0};
    bool operator==(const Rid&) const = default;
};
} // namespace minisql
