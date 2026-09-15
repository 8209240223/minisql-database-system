#pragma once
#include "minisql/storage/buffer_pool.hpp"
#include <functional>
#include <optional>
#include <string>
#include <variant>

namespace minisql::storage {
enum class ColumnType { Int = 0, Varchar = 1, Bigint = 2, Decimal = 3, Bool = 4, Date = 5, BoundedVarchar = 6, Float = 7 };
// 列类型枚举；数值与磁盘格式绑定，改动会破坏兼容性。
struct ColumnSchema {
// 一列的模式信息：类型以及该类型需要的附加参数。
    ColumnType type;
    // 列类型。
    unsigned precision = 0, scale = 0;
    // DECIMAL 的精度与标度，其他类型为 0。
    std::uint32_t maxLength = 0;
    // 带长度 VARCHAR 的字符上限。
    ColumnSchema(ColumnType value) : type(value) {}
    // 由类型构造，附加参数取默认值。
    ColumnSchema(unsigned p, unsigned s) : type(ColumnType::Decimal), precision(p), scale(s) {}
    // 由精度标度构造，自动把类型设为 DECIMAL。
    static ColumnSchema varchar(std::uint32_t length) { ColumnSchema result{ColumnType::BoundedVarchar};result.maxLength=length;return result; }
    // 构造带长度限制的 VARCHAR 列。
    bool operator==(ColumnType other) const { return type == other; }
    // 允许直接与类型枚举比较，写起来更简洁。
    bool operator==(const ColumnSchema&) const = default;
    // 让编译器生成完整的相等比较，用于校验模式一致性。
};
using RowSchema = std::vector<ColumnSchema>;
// 一张表的模式就是一串列定义。
using Value = std::variant<std::int32_t, std::string, std::monostate, std::int64_t, bool, double>;
// 一个单元格的取值：i32、字符串、空值标记、i64、布尔、双精度浮点。
using Row = std::vector<Value>;
// 一行就是一串单元格。
struct RowRef { PageRef page; SlotRef slot; };
// 行引用：定位一行需要页引用与槽引用。
std::vector<std::uint8_t> encodeRow(const Row& row, const RowSchema& schema);
// 把一行按模式编码成字节，这是“行到页”的序列化。
Row decodeRow(std::span<const std::uint8_t> bytes, const RowSchema& schema);
// 把字节按模式还原成一行，这是反序列化。

class HeapStore {
// 堆表存储：以无序堆的方式把行放进页里，插入快、扫描靠遍历。
public:
    HeapStore(std::shared_ptr<PageFile> file, BufferPool& buffer) : file_(std::move(file)), buffer_(buffer) {}
    // 构造：只需要页文件与缓冲池，两者都由外部共享。
    RowRef insert(std::uint64_t table, const RowSchema& schema, const Row& row);
    // 插入一行，返回行引用。
    Row read(std::uint64_t table, const RowSchema& schema, RowRef ref);
    // 按引用读出一行，并校验它确实属于指定表。
    void erase(std::uint64_t table, RowRef ref);
    // 删除一行；如果所在页空了就把整页交还给页文件。
    RowRef replace(std::uint64_t table, const RowSchema& schema, RowRef ref, const Row& row);
    // 原地替换一行：先插入新行再删除旧行，返回新的行引用。
    void scan(std::uint64_t table, const RowSchema& schema, const std::function<void(RowRef, const Row&)>& visitor);
    // 全表扫描：逐页逐槽读取并回调给上层。
    std::vector<RowRef> refsFor(std::uint64_t table);
    // 顺序扫描游标：按行引用顺序读行，同一页的连续行只取一次页。
    // 存在的理由：此前每个行都单独调一次 read()，而 read() 内部每次都取页，
    // 导致页访问次数与行数同阶（实测 4000 行 4000 次），而实际只需要页数那么多次。
    class RowCursor {
    public:
        RowCursor(HeapStore& heap, std::uint64_t table, const RowSchema& schema, std::vector<RowRef> refs);
        // 构造：绑定堆、表、行结构与待读的行引用快照。
        bool next(Row& row);
        // 取下一行；读完返回 false。
        std::size_t pageFetches() const { return fetches_; }
        // 实际取页次数，用来验证优化是否生效。
    private:
        HeapStore& heap_;
        // 所属堆存储。
        std::uint64_t table_;
        // 表编号，读行时用于校验归属。
        const RowSchema& schema_;
        // 行结构，解码时使用。
        std::vector<RowRef> refs_;
        // 全部待读行的引用快照。
        std::size_t cursor_ = 0;
        // 当前读到第几条。
        std::size_t fetches_ = 0;
        // 已取页次数。
        std::optional<PageGuard> guard_;
        // 当前持有页的使用凭证；为空表示还没有页在手。
        PageId residentPage_ = 0;
        // 当前凭证对应哪一页；0 表示无效（页号从 1 开始）。
    };

    // 只收集行引用不读取内容，供索引重建等场景使用。
    void flush() { buffer_.flushAll(); }
    // 把缓冲池里的改动全部刷盘。
private:
    friend class RowCursor;
    // RowCursor 需要直接取页（buffer_.get），因此授予友元；
    // 这样可以把"取页"留在游标内部，避免每行都走一次公开 read()。
    std::shared_ptr<PageFile> file_;
    // 页文件，用于查询某张表有哪些页。
    BufferPool& buffer_;
    // 缓冲池引用，通过它取页与分配页。
};
}
