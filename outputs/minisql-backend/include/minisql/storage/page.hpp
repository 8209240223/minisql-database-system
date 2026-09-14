#pragma once
#include "minisql/common/types.hpp"
#include <array>
#include <span>
#include <vector>

namespace minisql::storage {
inline constexpr std::size_t kPageSize = 4096;
// 一页固定 4096 字节，这是全部页内布局与文件偏移计算的基础。
using PageBytes = std::array<std::uint8_t, kPageSize>;
// 页的字节序列类型：长度固定为 4096 的字节数组。
struct SlotRef {
// 槽引用：定位页内一条记录需要“第几个槽”加“该槽的代数”。
    std::uint32_t slot;
    // 槽号，从 0 开始。
    std::uint64_t generation;
    // 代数：槽被删除后重新插入会加一，用于识别过期引用。
};

class SlottedPage {
// 槽式页：页头加槽目录从前往后增长，记录内容从页尾往前存放。
public:
    SlottedPage(PageId id, std::uint64_t owner, std::uint64_t generation = 1);
    // 新建一张空白页，需要给出页号、归属者（哪张表）与代数。
    explicit SlottedPage(PageBytes bytes);
    // 从磁盘读到的字节还原成页，构造函数里会校验校验和。explicit 禁止隐式转换。
    PageId id() const;
    // 取页号。
    std::uint64_t owner() const;
    // 取归属者标识，用于判断这页属于哪张表或哪个索引。
    std::uint64_t generation() const;
    // 取页的代数。
    SlotRef insert(std::span<const std::uint8_t> record);
    // 插入一条记录，返回可长期保存的槽引用；页满会抛错。
    std::vector<std::uint8_t> read(SlotRef reference) const;
    // 按槽引用读出记录内容。
    void erase(SlotRef reference);
    // 按槽引用删除记录，只把槽标记为空并将空间留给后续整理。
    std::vector<SlotRef> liveSlots() const;
    // 列出所有仍然有效的槽，扫描表时用它遍历记录。
    std::size_t freeSpace() const;
    // 计算当前可用空闲字节数。
    bool canInsert(std::size_t bytes) const;
    // 预判这条记录是否放得下，避免插入时才失败。
    PageBytes serialize() const;
    // 序列化：先校验再写入校验和，得到可落盘的字节。
private:
    PageBytes bytes_{};
    // 页的真实内容，全部操作都是在这个数组上读写。
    void validate() const;
    // 校验页头与槽目录的自洽性，发现损坏即报错。
    void requireLive(SlotRef reference) const;
    // 确认槽引用仍然有效且代数匹配。
    void compact();
    // 整理页内空间，把记录重新紧凑排列到页尾。
};

std::uint64_t readUnsigned(const PageBytes& bytes, std::size_t offset, std::size_t width);
// 从页中指定偏移读出 width 字节的小端无符号整数。
void writeUnsigned(PageBytes& bytes, std::size_t offset, std::size_t width, std::uint64_t value);
// 把无符号整数按小端写入指定偏移，写入前检查是否越界或溢出。
std::uint32_t checksum(const PageBytes& bytes);
// 计算页校验和，目前是 FNV-1a 变体；存储与读取时用它检测损坏。
}
