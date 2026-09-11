#pragma once
#include "minisql/storage/buffer_pool.hpp"
#include <functional>
#include <string>
#include <variant>

namespace minisql::storage {
enum class ColumnType { Int = 0, Varchar = 1, Bigint = 2, Decimal = 3, Bool = 4, Date = 5, BoundedVarchar = 6, Float = 7 };
struct ColumnSchema {
    ColumnType type;
    unsigned precision = 0, scale = 0;
    std::uint32_t maxLength = 0;
    ColumnSchema(ColumnType value) : type(value) {}
    ColumnSchema(unsigned p, unsigned s) : type(ColumnType::Decimal), precision(p), scale(s) {}
    static ColumnSchema varchar(std::uint32_t length) { ColumnSchema result{ColumnType::BoundedVarchar};result.maxLength=length;return result; }
    bool operator==(ColumnType other) const { return type == other; }
    bool operator==(const ColumnSchema&) const = default;
};
using RowSchema = std::vector<ColumnSchema>;
using Value = std::variant<std::int32_t, std::string, std::monostate, std::int64_t, bool, double>;
using Row = std::vector<Value>;
struct RowRef { PageRef page; SlotRef slot; };
std::vector<std::uint8_t> encodeRow(const Row& row, const RowSchema& schema);
Row decodeRow(std::span<const std::uint8_t> bytes, const RowSchema& schema);

class HeapStore {
public:
    HeapStore(std::shared_ptr<PageFile> file, BufferPool& buffer) : file_(std::move(file)), buffer_(buffer) {}
    RowRef insert(std::uint64_t table, const RowSchema& schema, const Row& row);
    Row read(std::uint64_t table, const RowSchema& schema, RowRef ref);
    void erase(std::uint64_t table, RowRef ref);
    RowRef replace(std::uint64_t table, const RowSchema& schema, RowRef ref, const Row& row);
    void scan(std::uint64_t table, const RowSchema& schema, const std::function<void(RowRef, const Row&)>& visitor);
    std::vector<RowRef> refsFor(std::uint64_t table);
    void flush() { buffer_.flushAll(); }
private:
    std::shared_ptr<PageFile> file_;
    BufferPool& buffer_;
};
}
