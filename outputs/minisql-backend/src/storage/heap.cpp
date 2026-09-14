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
// 编译期断言：双精度必须是 8 字节且符合 IEEE754，否则下面的位搬运不成立。
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
// 统一的存储错误出口。
void append32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
// 把 32 位整数按小端追加到字节流末尾。
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
    // 依次取最低字节到最高字节。
}
std::uint32_t take32(std::span<const std::uint8_t> bytes, std::size_t& offset) {
// 从字节流当前位置读取 32 位整数，并把游标推进 4 字节。
    if (offset > bytes.size() || bytes.size() - offset < 4) fail("STORAGE_CORRUPTION: truncated row field");
    // 剩余不足 4 字节说明记录被截断。
    std::uint32_t value = 0;
    // 累加结果。
    for (int i = 0; i < 4; ++i) value |= std::uint32_t(bytes[offset++]) << (i * 8);
    // 按小端拼装，同时推进游标。
    return value;
    // 返回读到的值。
}
std::uint32_t descriptor(const ColumnSchema& column) {
// 把列模式压成一个 32 位描述符，写入记录头供解码时校验。
    if ((column == ColumnType::BoundedVarchar) != (column.maxLength != 0)) fail("Invalid VARCHAR length descriptor");
    // 带长度 VARCHAR 必须有长度，其他类型必须没有，二者要一致。
    if (column == ColumnType::Decimal) {
    // DECIMAL 需要额外检查精度标度。
        if (column.precision == 0 || column.precision > 38 || column.scale > column.precision) fail("Invalid DECIMAL row schema");
        // 精度必须在 1 到 38 之间，标度不能超过精度。
    } else if ((column != ColumnType::Int && column != ColumnType::Varchar && column != ColumnType::Bigint && column != ColumnType::Bool && column != ColumnType::Date && column != ColumnType::BoundedVarchar && column != ColumnType::Float) || column.precision || column.scale) fail("Invalid row schema");
    // 非 DECIMAL 的类型不允许带精度标度，且类型本身必须受支持。
    return static_cast<std::uint32_t>(column.type) | (column.precision << 8) | (column.scale << 16);
    // 低 8 位放类型，接着 8 位放精度，再 8 位放标度。
}
}
std::vector<std::uint8_t> encodeRow(const Row& row, const RowSchema& schema) {
// 行编码：把一行值按模式写成字节序列，包含版本、列数、NULL 位图、类型描述与各列数据。
    if (row.size() != schema.size() || schema.empty() || schema.size() > 128) fail("Row/schema column count mismatch");
    // 值的个数必须与列数一致；列数上限 128 是格式约束。
    std::vector<std::uint8_t> bytes;
    // 输出缓冲。
    const bool bounded = std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::BoundedVarchar; });
    // 是否含带长度 VARCHAR 列。
    const bool typed = bounded || std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::Decimal || column == ColumnType::Bool || column == ColumnType::Date || column == ColumnType::Float; });
    // 是否需要写入类型描述符：带长度字符串或需要额外参数的类型都算。
    const bool nullable = typed || std::any_of(row.begin(), row.end(), [](const auto& value) { return std::holds_alternative<std::monostate>(value); });
    // 是否需要 NULL 位图：需要描述符时一律写位图，或者实际出现了空值。
    append32(bytes, bounded ? 4 : typed ? 3 : nullable ? 2 : 1);
    // 行版本号：4 表示带长度与类型描述，3 表示带类型描述，2 表示带 NULL 位图，1 表示最简格式。
    append32(bytes, static_cast<std::uint32_t>(schema.size()));
    // 写入列数，解码时与模式比对。
    if (nullable) {
    // 需要位图时先预留空间。
        bytes.resize(8 + (schema.size() + 7) / 8, 0);
        // 前 8 字节是已经写好的版本与列数，之后按每列一位预留。
        for (std::size_t i = 0; i < row.size(); ++i)
        // 逐列检查是否为空。
            if (std::holds_alternative<std::monostate>(row[i])) bytes[8 + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
            // 第 i 列为空就把位图对应位置 1。
    }
    for (const auto& column : schema) {
    // 写入每列的类型描述。
        const auto code = descriptor(column);
        // 计算描述符。
        if (typed) append32(bytes, code);
        // 需要描述符的格式才写。
        if (column == ColumnType::BoundedVarchar) append32(bytes,column.maxLength);
        // 带长度字符串还要额外写长度上限。
    }
    for (std::size_t i = 0; i < schema.size(); ++i) {
    // 逐列写数据。
        if (std::holds_alternative<std::monostate>(row[i])) continue;
        // 空值不写数据，位图里已经标记。
        if (schema[i] == ColumnType::Int && std::holds_alternative<std::int32_t>(row[i])) {
        // INT 列。
            append32(bytes, std::bit_cast<std::uint32_t>(std::get<std::int32_t>(row[i])));
            // 用位搬运写入 4 字节，负数也保持位模式。
        } else if (schema[i] == ColumnType::Bigint && std::holds_alternative<std::int64_t>(row[i])) {
        // BIGINT 列。
            const auto value = std::bit_cast<std::uint64_t>(std::get<std::int64_t>(row[i]));
            // 先转成无符号位模式。
            append32(bytes, static_cast<std::uint32_t>(value));
            // 低 32 位。
            append32(bytes, static_cast<std::uint32_t>(value >> 32));
            // 高 32 位。
        } else if (schema[i] == ColumnType::Float && std::holds_alternative<double>(row[i])) {
        // FLOAT 列。
            const auto value = std::get<double>(row[i]);
            // 取值。
            if (!std::isfinite(value)) fail("FLOAT storage requires a finite value");
            // 无穷与 NaN 不允许落盘。
            const auto bits = std::bit_cast<std::uint64_t>(value);
            // 取 IEEE754 位模式。
            append32(bytes, static_cast<std::uint32_t>(bits));
            // 低 32 位。
            append32(bytes, static_cast<std::uint32_t>(bits >> 32));
            // 高 32 位。
        } else if (schema[i] == ColumnType::Bool && std::holds_alternative<bool>(row[i])) {
        // BOOL 列。
            bytes.push_back(std::get<bool>(row[i]) ? 1 : 0);
            // 用一个字节表示真假。
        } else if (schema[i] == ColumnType::Date && std::holds_alternative<std::string>(row[i])) {
        // DATE 列。
            append32(bytes,std::bit_cast<std::uint32_t>(parseIsoDate(std::get<std::string>(row[i]),{},ErrorCode::Storage)));
            // 把日期文本解析成天数后写入 4 字节。
        } else if (schema[i] == ColumnType::Decimal && std::holds_alternative<std::string>(row[i])) {
        // DECIMAL 列。
            try {
                auto units = ExactDecimal::parse(std::get<std::string>(row[i]), schema[i].precision, schema[i].scale).coefficient();
                // 按列的精度标度解析出整数系数。
                if (units < 0) units += ExactDecimal::Integer(1) << 128;
                // 负数转成 128 位二进制补码表示。
                for (unsigned byte = 0; byte < 16; ++byte) {
                // 固定写 16 字节。
                    bytes.push_back(static_cast<std::uint8_t>((units & 255).convert_to<unsigned>()));units >>= 8;
                    // 取最低字节写入后右移 8 位。
                }
            } catch (const MiniSqlError&) { fail("DECIMAL row value does not fit schema"); }
            // 解析失败说明数值超出该列的精度范围。
        } else if ((schema[i] == ColumnType::Varchar || schema[i] == ColumnType::BoundedVarchar) && std::holds_alternative<std::string>(row[i])) {
        // 字符串列。
            const auto& value = std::get<std::string>(row[i]);
            // 取字符串引用。
            if (value.size() > 4016 || bytes.size() + 4 + value.size() > 4016) fail("ROW_TOO_LARGE");
            // 单列不能超过页内可用空间，整行也不能。
            if (schema[i] == ColumnType::BoundedVarchar) validateVarchar(value,schema[i].maxLength,{},ErrorCode::Storage);
            // 带长度限制的列按字符数校验。
            else (void)utf8Length(value);
            // 不带长度的也要校验 UTF-8 合法性。
            append32(bytes, static_cast<std::uint32_t>(value.size()));
            // 先写字符串长度（字节数）。
            bytes.insert(bytes.end(), value.begin(), value.end());
            // 再写字符串内容本身。
        } else fail("Row/schema type mismatch");
        // 值类型与列类型不匹配，属于调用方错误。
        if (bytes.size() > 4016) fail("ROW_TOO_LARGE");
        // 每写一列都检查总长度，避免整行超过一页。
    }
    return bytes;
    // 返回编码结果。
}
Row decodeRow(std::span<const std::uint8_t> bytes, const RowSchema& schema) {
// 行解码：按模式把字节还原成一行值，并逐项做损坏检查。
    std::size_t offset = 0;
    // 读取游标。
    const auto version = take32(bytes, offset);
    // 读版本号。
    if ((version != 1 && version != 2 && version != 3 && version != 4) || take32(bytes, offset) != schema.size() || schema.empty() || schema.size() > 128)
    // 版本必须是 1 到 4，列数必须与模式一致，列数上限 128。
        fail("STORAGE_CORRUPTION: row version or schema");
    const bool bounded = std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::BoundedVarchar; });
    // 模式里是否含带长度字符串。
    const bool typed = bounded || std::any_of(schema.begin(), schema.end(), [](const auto& column) { return column == ColumnType::Decimal || column == ColumnType::Bool || column == ColumnType::Date || column == ColumnType::Float; });
    // 模式是否需要类型描述符。
    if ((version >= 3) != typed || (version == 4) != bounded) fail("STORAGE_CORRUPTION: typed row version mismatch");
    // 版本号必须与模式特征一致，否则说明字节与模式对不上。
    if (version >= 2) {
    // 版本 2 及以上带 NULL 位图。
        offset += (schema.size() + 7) / 8;
        // 跳过位图占用的字节数。
        if (offset > bytes.size()) fail("STORAGE_CORRUPTION: truncated NULL bitmap");
        // 位图都不完整，记录显然被截断。
        if (schema.size() % 8 && (bytes[offset - 1] >> (schema.size() % 8))) fail("STORAGE_CORRUPTION: NULL bitmap padding");
        // 位图末尾未使用的填充位必须为零。
    }
    for (const auto& column : schema) {
    // 校验各列的类型描述符。
        const auto code = descriptor(column);
        // 由模式算出应有的描述符。
        if (version >= 3 && take32(bytes, offset) != code) fail("STORAGE_CORRUPTION: row type descriptor mismatch");
        // 与记录里的描述符比对。
        if (column == ColumnType::BoundedVarchar && take32(bytes,offset) != column.maxLength) fail("STORAGE_CORRUPTION: VARCHAR length descriptor mismatch");
        // 带长度字符串还要比对长度上限。
    }
    Row row;
    // 结果行。
    for (std::size_t i = 0; i < schema.size(); ++i) {
    // 逐列解码。
        const auto type = schema[i];
        // 当前列类型。
        if (version >= 2 && (bytes[8 + i / 8] & (1u << (i % 8)))) { row.emplace_back(std::monostate{}); continue; }
        // 位图标记为空，直接放入空值占位符。
        if (type == ColumnType::Bool) {
        // BOOL 列只占一字节。
            if (offset >= bytes.size() || bytes[offset] > 1) fail("STORAGE_CORRUPTION: invalid BOOL payload");
            // 越界或取值不是 0/1 都算损坏。
            row.emplace_back(bytes[offset++] != 0);
            // 转成布尔并推进游标。
            continue;
            // 本列处理完。
        }
        if (type == ColumnType::Decimal) {
        // DECIMAL 列固定 16 字节补码。
            if (bytes.size() - offset < 16) fail("STORAGE_CORRUPTION: truncated DECIMAL coefficient");
            // 剩余不足 16 字节说明记录被截断。
            ExactDecimal::Integer units = 0;
            // 累加系数。
            for (unsigned byte = 0; byte < 16; ++byte) units += ExactDecimal::Integer(bytes[offset + byte]) << (byte * 8);
            // 按小端拼出 128 位整数。
            if (bytes[offset + 15] & 128) units -= ExactDecimal::Integer(1) << 128;
            // 最高字节的符号位为 1 说明是负数，减去 2 的 128 次方还原补码。
            offset += 16;
            // 推进游标。
            try { row.emplace_back(ExactDecimal::fromCoefficient(std::move(units), type.precision, type.scale).format()); }
            // 还原成十进制文本放入行。
            catch (const MiniSqlError&) { fail("STORAGE_CORRUPTION: DECIMAL precision overflow"); }
            // 系数超过该列精度说明数据损坏。
            continue;
            // 本列处理完。
        }
        auto value = take32(bytes, offset);
        // 其余类型都以 4 字节整数开头。
        if (type == ColumnType::Date) row.emplace_back(formatIsoDate(std::bit_cast<std::int32_t>(value)));
        // DATE：天数还原成 YYYY-MM-DD 文本。
        else if (type == ColumnType::Int) row.emplace_back(std::bit_cast<std::int32_t>(value));
        // INT：直接按 32 位有符号解释。
        else if (type == ColumnType::Float) {
        // FLOAT：再读高 32 位拼成 64 位。
            const auto high = take32(bytes, offset);
            // 高 32 位。
            const auto number = std::bit_cast<double>((std::uint64_t(high) << 32) | value);
            // 拼成 64 位后按位还原成双精度。
            if (!std::isfinite(number)) fail("STORAGE_CORRUPTION: non-finite FLOAT payload");
            // 磁盘上不应该出现无穷或 NaN。
            row.emplace_back(number);
            // 放入行。
        }
        else if (type == ColumnType::Bigint) {
        // BIGINT：同样需要高 32 位。
            const auto high = take32(bytes, offset);
            // 高 32 位。
            row.emplace_back(std::bit_cast<std::int64_t>((std::uint64_t(high) << 32) | value));
            // 拼成 64 位有符号整数。
        }
        else if (type == ColumnType::Varchar || type == ColumnType::BoundedVarchar) {
        // 字符串：前面读到的 value 是字节长度。
            if (value > bytes.size() - offset) fail("STORAGE_CORRUPTION: string length");
            // 长度超过剩余字节说明损坏。
            std::string text(reinterpret_cast<const char*>(bytes.data() + offset), value);
            // 按长度取出字符串内容。
            if (type == ColumnType::BoundedVarchar) validateVarchar(text,type.maxLength,{},ErrorCode::Storage);
            // 带长度限制的列再校验一次字符数。
            else (void)utf8Length(text);
            // 普通字符串校验 UTF-8 合法性。
            row.emplace_back(std::move(text));
            // 放入行。
            offset += value;
            // 游标跳过字符串内容。
        } else fail("Unknown row column type");
        // 未知类型说明模式或数据有问题。
    }
    if (offset != bytes.size()) fail("STORAGE_CORRUPTION: trailing row bytes");
    // 解码完必须刚好用光所有字节，多出来说明记录被污染。
    return row;
    // 返回还原出的行。
}
RowRef HeapStore::insert(std::uint64_t table, const RowSchema& schema, const Row& row) {
// 插入一行：先编码，再从已有页里找空位，找不到就新分配一页。
    const auto bytes = encodeRow(row, schema);
    // 编码成字节。
    for (auto ref : file_->pagesFor(table)) {
    // 遍历这张表已分配的所有页。
        auto page = buffer_.get(ref);
        // 通过缓冲池取页。
        if (page.page().canInsert(bytes.size())) return {ref, page.insert(bytes)};
        // 放得下就插入并返回行引用。
    }
    const auto ref = buffer_.allocate(table);
    // 所有已有页都放不下，分配一张新页。
    auto page = buffer_.get(ref);
    // 取到新页。
    return {ref, page.insert(bytes)};
    // 插入并返回行引用。
}
Row HeapStore::read(std::uint64_t table, const RowSchema& schema, RowRef ref) {
// 按行引用读出一行。
    auto page = buffer_.get(ref.page);
    // 取所在页。
    if (page.page().owner() != table) fail("Row belongs to another table");
    // 校验页的归属，防止跨表误读。
    return decodeRow(page.page().read(ref.slot), schema);
    // 读出槽内容并解码。
}
void HeapStore::erase(std::uint64_t table, RowRef ref) {
// 删除一行；如果这一页空了就连页一起回收。
    bool empty;
    // 记录删除后该页是否已无有效行。
    {
        auto page = buffer_.get(ref.page);
        // 单独用一对花括号限定作用域，让凭证在这里析构，pin 及时释放。
        if (page.page().owner() != table) fail("Row belongs to another table");
        // 归属校验。
        page.erase(ref.slot);
        // 删除槽内容。
        empty = page.page().liveSlots().empty();
        // 检查是否还有有效槽。
    }
    if (empty) buffer_.release(ref.page);
    // 页空了就释放整页，把页号交还空闲列表。
}
RowRef HeapStore::replace(std::uint64_t table, const RowSchema& schema, RowRef ref, const Row& row) {
// 替换一行：先确认旧行存在，再插入新行、删除旧行。
    (void)read(table, schema, ref);
    // 先读一次旧行，确保引用有效、归属正确；(void) 表示只用副作用。
    // 先保存新记录再移除旧记录；迁移后的 RowRef 必须交给未来的索引维护路径。
    const auto replacement = insert(table, schema, row);
    // 插入新行，拿到新的行引用。
    erase(table, ref);
    // 删除旧行。
    return replacement;
    // 返回新引用，调用方据此更新索引。
}
void HeapStore::scan(std::uint64_t table, const RowSchema& schema, const std::function<void(RowRef, const Row&)>& visitor) {
// 全表扫描：逐页逐槽解码，并把每一行回调给上层。
    for (auto ref : file_->pagesFor(table)) {
    // 遍历该表所有页。
        auto page = buffer_.get(ref);
        // 取页。
        for (auto slot : page.page().liveSlots()) {
        // 遍历页内所有有效槽。
            const auto row = decodeRow(page.page().read(slot), schema);
            // 解码这一行。
            visitor({ref, slot}, row);
            // 交给回调处理，例如做过滤或投影。
        }
    }
}

std::vector<RowRef> HeapStore::refsFor(std::uint64_t table) {
// 收集该表所有行的引用，不读取内容。
    std::vector<RowRef> refs;
    // 结果集合。
    for (const auto ref : file_->pagesFor(table)) {
    // 遍历所有页。
        const auto page = buffer_.get(ref);
        // 取页。
        for (const auto slot : page.page().liveSlots()) refs.push_back({ref, slot});
        // 每个有效槽生成一个行引用。
    }
    return refs;
    // 返回收集结果。
}
}
