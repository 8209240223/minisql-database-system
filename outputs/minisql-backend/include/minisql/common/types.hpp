#pragma once
#include <cstdint>
#include <limits>

namespace minisql {
using PageId = std::uint64_t;
// 页号类型：64 位无符号整数，页在数据库文件中的唯一编号。
using TransactionId = std::uint64_t;
// 事务号类型：用于标识一次写事务。
using Lsn = std::uint64_t;
// 日志序号类型：日志文件中的字节位置。
inline constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();
// 无效页号常量：取 64 位最大值，正常分配的页号不会等于它。
struct Rid {
// 行标识：记录定位一条记录需要“哪一页、页内第几个槽”。
    PageId pageId{kInvalidPageId};
    // 记录所在的页号，默认是无效页号。
    std::uint32_t slotId{0};
    // 记录在该页的槽号，从 0 开始。
    bool operator==(const Rid&) const = default;
    // 让编译器自动生成相等比较，保证比较的是两个字段的合体。
};
} // namespace minisql
