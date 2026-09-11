#include "minisql/storage/heap.hpp"
#include "minisql/common/error.hpp"
#include "minisql/common/decimal.hpp"
#include "minisql/common/date.hpp"
#include "minisql/common/varchar.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <algorithm>

namespace minisql::storage {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::uint32_t take32(std::span<const std::uint8_t> bytes, std::size_t& offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) fail("STORAGE_CORRUPTION: truncated row field");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= std::uint32_t(bytes[offset++]) << (i * 8);
    return value;
}
std::uint32_t descriptor(const ColumnSchema& column) {
    if ((column == ColumnType::BoundedVarchar) != (column.maxLength != 0)) fail("Invalid VARCHAR length descriptor");
    if (column == ColumnType::Decimal) {
        if (column.precision == 0 || column.precision > 38 || column.scale > column.precision) fail("Invalid DECIMAL row schema");
    } else if ((column != ColumnType::Int && column != ColumnType::Varchar && column != ColumnType::Bigint && column != ColumnType::Bool && column != ColumnType::Date && column != ColumnType::BoundedVarchar && column != ColumnType::Float) || column.precision || column.scale) fail("Invalid row schema");
    return static_cast<std::uint32_t>(column.type) | (column.precision << 8) | (column.scale << 16);
}
}
std::vector<std::uint8_t> encodeRow(const Row& row, const RowSchema& schema) {
    if (row.size() != schema.size() || schema.empty() || schema.size() > 128) fail("Row/schema column count mismatch");
    std::vector<std::uint8_t> bytes;
    const bool bounded = std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::BoundedVarchar; });
    const bool typed = bounded || std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::Decimal || column == ColumnType::Bool || column == ColumnType::Date || column == ColumnType::Float; });
    const bool nullable = typed || std::any_of(row.begin(), row.end(), [](const auto& value) { return std::holds_alternative<std::monostate>(value); });
    append32(bytes, bounded ? 4 : typed ? 3 : nullable ? 2 : 1);
    append32(bytes, static_cast<std::uint32_t>(schema.size()));
    if (nullable) {
        bytes.resize(8 + (schema.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < row.size(); ++i)
            if (std::holds_alternative<std::monostate>(row[i])) bytes[8 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    }
    for (const auto& column : schema) {
        const auto code = descriptor(column);
        if (typed) append32(bytes, code);
        if (column == ColumnType::BoundedVarchar) append32(bytes,column.maxLength);
    }
    for (std::size_t i = 0; i < schema.size(); ++i) {
        if (std::holds_alternative<std::monostate>(row[i])) continue;
        if (schema[i] == ColumnType::Int && std::holds_alternative<std::int32_t>(row[i])) {
            append32(bytes, std::bit_cast<std::uint32_t>(std::get<std::int32_t>(row[i])));
        } else if (schema[i] == ColumnType::Bigint && std::holds_alternative<std::int64_t>(row[i])) {
            const auto value = std::bit_cast<std::uint64_t>(std::get<std::int64_t>(row[i]));
            append32(bytes, static_cast<std::uint32_t>(value));
            append32(bytes, static_cast<std::uint32_t>(value >> 32));
        } else if (schema[i] == ColumnType::Float && std::holds_alternative<double>(row[i])) {
            const auto value = std::get<double>(row[i]);
            if (!std::isfinite(value)) fail("FLOAT storage requires a finite value");
            const auto bits = std::bit_cast<std::uint64_t>(value);
            append32(bytes, static_cast<std::uint32_t>(bits));
            append32(bytes, static_cast<std::uint32_t>(bits >> 32));
        } else if (schema[i] == ColumnType::Bool && std::holds_alternative<bool>(row[i])) {
            bytes.push_back(std::get<bool>(row[i]) ? 1 : 0);
        } else if (schema[i] == ColumnType::Date && std::holds_alternative<std::string>(row[i])) {
            append32(bytes,std::bit_cast<std::uint32_t>(parseIsoDate(std::get<std::string>(row[i]),{},ErrorCode::Storage)));
        } else if (schema[i] == ColumnType::Decimal && std::holds_alternative<std::string>(row[i])) {
            try {
                auto units = ExactDecimal::parse(std::get<std::string>(row[i]), schema[i].precision, schema[i].scale).coefficient();
                if (units < 0) units += ExactDecimal::Integer(1) << 128;
                for (unsigned byte = 0; byte < 16; ++byte) {
                    bytes.push_back(static_cast<std::uint8_t>((units & 255).convert_to<unsigned>()));units >>= 8;
                }
            } catch (const MiniSqlError&) { fail("DECIMAL row value does not fit schema"); }
        } else if ((schema[i] == ColumnType::Varchar || schema[i] == ColumnType::BoundedVarchar) && std::holds_alternative<std::string>(row[i])) {
            const auto& value = std::get<std::string>(row[i]);
            if (value.size() > 4016 || bytes.size() + 4 + value.size() > 4016) fail("ROW_TOO_LARGE");
            if (schema[i] == ColumnType::BoundedVarchar) validateVarchar(value,schema[i].maxLength,{},ErrorCode::Storage);
            else (void)utf8Length(value);
            append32(bytes, static_cast<std::uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        } else fail("Row/schema type mismatch");
        if (bytes.size() > 4016) fail("ROW_TOO_LARGE");
    }
    return bytes;
}
Row decodeRow(std::span<const std::uint8_t> bytes, const RowSchema& schema) {
    std::size_t offset = 0;
    const auto version = take32(bytes, offset);
    if ((version != 1 && version != 2 && version != 3 && version != 4) || take32(bytes, offset) != schema.size() || schema.empty() || schema.size() > 128)
        fail("STORAGE_CORRUPTION: row version or schema");
    const bool bounded = std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::BoundedVarchar; });
    const bool typed = bounded || std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::Decimal || column == ColumnType::Bool || column == ColumnType::Date || column == ColumnType::Float; });
    if ((version >= 3) != typed || (version == 4) != bounded) fail("STORAGE_CORRUPTION: typed row version mismatch");
    if (version >= 2) {
        offset += (schema.size() + 7) / 8;
        if (offset > bytes.size()) fail("STORAGE_CORRUPTION: truncated NULL bitmap");
        if (schema.size() % 8 && (bytes[offset - 1] >> (schema.size() % 8))) fail("STORAGE_CORRUPTION: NULL bitmap padding");
    }
    for (const auto& column : schema) {
        const auto code = descriptor(column);
        if (version >= 3 && take32(bytes, offset) != code) fail("STORAGE_CORRUPTION: row type descriptor mismatch");
        if (column == ColumnType::BoundedVarchar && take32(bytes,offset) != column.maxLength) fail("STORAGE_CORRUPTION: VARCHAR length descriptor mismatch");
    }
    Row row;
    for (std::size_t i = 0; i < schema.size(); ++i) {
        const auto type = schema[i];
        if (version >= 2 && (bytes[8 + i / 8] & (1u << (i % 8)))) { row.emplace_back(std::monostate{}); continue; }
        if (type == ColumnType::Bool) {
            if (offset >= bytes.size() || bytes[offset] > 1) fail("STORAGE_CORRUPTION: invalid BOOL payload");
            row.emplace_back(bytes[offset++] != 0);
            continue;
        }
        if (type == ColumnType::Decimal) {
            if (bytes.size() - offset < 16) fail("STORAGE_CORRUPTION: truncated DECIMAL coefficient");
            ExactDecimal::Integer units = 0;
            for (unsigned byte = 0; byte < 16; ++byte) units += ExactDecimal::Integer(bytes[offset + byte]) << (byte * 8);
            if (bytes[offset + 15] & 128) units -= ExactDecimal::Integer(1) << 128;
            offset += 16;
            try { row.emplace_back(ExactDecimal::fromCoefficient(std::move(units), type.precision, type.scale).format()); }
            catch (const MiniSqlError&) { fail("STORAGE_CORRUPTION: DECIMAL precision overflow"); }
            continue;
        }
        auto value = take32(bytes, offset);
        if (type == ColumnType::Date) row.emplace_back(formatIsoDate(std::bit_cast<std::int32_t>(value)));
        else if (type == ColumnType::Int) row.emplace_back(std::bit_cast<std::int32_t>(value));
        else if (type == ColumnType::Float) {
            const auto high = take32(bytes, offset);
            const auto number = std::bit_cast<double>((std::uint64_t(high) << 32) | value);
            if (!std::isfinite(number)) fail("STORAGE_CORRUPTION: non-finite FLOAT payload");
            row.emplace_back(number);
        }
        else if (type == ColumnType::Bigint) {
            const auto high = take32(bytes, offset);
            row.emplace_back(std::bit_cast<std::int64_t>((std::uint64_t(high) << 32) | value));
        }
        else if (type == ColumnType::Varchar || type == ColumnType::BoundedVarchar) {
            if (value > bytes.size() - offset) fail("STORAGE_CORRUPTION: string length");
            std::string text(reinterpret_cast<const char*>(bytes.data() + offset), value);
            if (type == ColumnType::BoundedVarchar) validateVarchar(text,type.maxLength,{},ErrorCode::Storage);
            else (void)utf8Length(text);
            row.emplace_back(std::move(text));
            offset += value;
        } else fail("Unknown row column type");
    }
    if (offset != bytes.size()) fail("STORAGE_CORRUPTION: trailing row bytes");
    return row;
}
RowRef HeapStore::insert(std::uint64_t table, const RowSchema& schema, const Row& row) {
    const auto bytes = encodeRow(row, schema);
    for (auto ref : file_->pagesFor(table)) {
        auto page = buffer_.get(ref);
        if (page.page().canInsert(bytes.size())) return {ref, page.insert(bytes)};
    }
    const auto ref = buffer_.allocate(table);
    auto page = buffer_.get(ref);
    return {ref, page.insert(bytes)};
}
Row HeapStore::read(std::uint64_t table, const RowSchema& schema, RowRef ref) {
    auto page = buffer_.get(ref.page);
    if (page.page().owner() != table) fail("Row belongs to another table");
    return decodeRow(page.page().read(ref.slot), schema);
}
void HeapStore::erase(std::uint64_t table, RowRef ref) {
    bool empty;
    {
        auto page = buffer_.get(ref.page);
        if (page.page().owner() != table) fail("Row belongs to another table");
        page.erase(ref.slot);
        empty = page.page().liveSlots().empty();
    }
    if (empty) buffer_.release(ref.page);
}
RowRef HeapStore::replace(std::uint64_t table, const RowSchema& schema, RowRef ref, const Row& row) {
    (void)read(table, schema, ref);
    // 先保存新记录再移除旧记录；迁移后的 RowRef 必须交给未来的索引维护路径。
    const auto replacement = insert(table, schema, row);
    erase(table, ref);
    return replacement;
}
void HeapStore::scan(std::uint64_t table, const RowSchema& schema, const std::function<void(RowRef, const Row&)>& visitor) {
    for (auto ref : file_->pagesFor(table)) {
        auto page = buffer_.get(ref);
        for (auto slot : page.page().liveSlots()) {
            const auto row = decodeRow(page.page().read(slot), schema);
            visitor({ref, slot}, row);
        }
    }
}

std::vector<RowRef> HeapStore::refsFor(std::uint64_t table) {
    std::vector<RowRef> refs;
    for (const auto ref : file_->pagesFor(table)) {
        const auto page = buffer_.get(ref);
        for (const auto slot : page.page().liveSlots()) refs.push_back({ref, slot});
    }
    return refs;
}
}
