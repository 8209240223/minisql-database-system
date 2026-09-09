#include "minisql/storage/heap.hpp"
#include "minisql/common/error.hpp"
#include <cstdlib>
#include <iostream>

using namespace minisql;
using namespace minisql::storage;
int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("mode and fixture path required");
        std::string mode = argv[1];const std::string point = argc > 3 ? argv[3] : "";
        const bool decimal = mode.starts_with("decimal-");
        const bool boolean = mode.starts_with("bool-");
        const bool date = mode.starts_with("date-");
        const bool varchar = mode.starts_with("varchar-");
        if (decimal) mode.erase(0, 8);
        if (boolean) mode.erase(0, 5);
        if (date) mode.erase(0, 5);
        if (varchar) mode.erase(0, 8);
        const auto path = std::filesystem::path(argv[2]);
        auto observer = [&](std::string_view stage) {
            if (stage != point) return;
            if (mode == "throw") throw MiniSqlError(ErrorCode::Storage, "Injected commit failure");
            std::_Exit(77);
        };
        auto file = std::make_shared<PageFile>(path, observer);
        BufferPool buffer(file, 1, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        const RowSchema schema{ColumnType::Int, varchar ? ColumnSchema::varchar(3) : date ? ColumnSchema{ColumnType::Date} : boolean ? ColumnSchema{ColumnType::Bool} : decimal ? ColumnSchema{38,6} : ColumnSchema{ColumnType::Varchar}};
        const auto valueFor = [&](std::int32_t id, bool updated) -> Value {
            if (id == 1) {
                if (date) return std::string(updated ? "2024-02-29" : "1970-01-01");
                if (boolean) return updated;
                return std::string(decimal ? (updated ? "2.000000" : "1.000000") : (updated ? "new" : "old"));
            }
            if (date || boolean || varchar) {
                if (id % 3 == 0) return std::monostate{};
                if (varchar) return std::string(id % 2 == 0 ? "\xe4\xb8\xad" : "\xf0\x9f\x98\x80");
                if (date) return std::string(id % 2 == 0 ? "0001-01-01" : "9999-12-31");
                return id % 2 == 0;
            }
            return decimal ? std::string("99999999999999999999999999999999.999999") : std::string(1500,'x');
        };
        if (mode == "init") {
            heap.insert(10, schema, {std::int32_t(1), valueFor(1,false)});heap.flush();
            const auto unused = buffer.allocate(20);buffer.release(unused);
        } else if (mode == "hold") {
            std::cout << "LOCKED\n" << std::flush;
            std::string input;std::getline(std::cin, input);
        } else if (mode == "empty") {
            buffer.beginWriteBatch();buffer.commitWriteBatch();
        } else if (mode == "commit" || mode == "throw") {
            std::vector<RowRef> refs;heap.scan(10, schema, [&](RowRef ref, const Row&) { refs.push_back(ref); });
            if (refs.size() != 1) throw std::runtime_error("expected base fixture");
            buffer.beginWriteBatch();
            const auto reused = buffer.allocate(30);buffer.release(reused);
            auto last = heap.replace(10, schema, refs[0], {std::int32_t(1), valueFor(1,true)});
            for (std::int32_t id = 2; id <= (decimal || boolean || date || varchar ? 240 : 8); ++id)
                last = heap.insert(10, schema, {id, valueFor(id,true)});
            if (mode == "throw") {
                bool failed = false;try { buffer.commitWriteBatch(); } catch (const MiniSqlError&) { failed = true; }
                if (!failed) throw std::runtime_error("fault not reached");
                if (point == "prepared") { buffer.rollbackWriteBatch();std::cout << "ROLLBACK_OK\n"; }
                else {
                    bool rollbackBlocked = false, cacheBlocked = false;
                    try { buffer.rollbackWriteBatch(); } catch (const MiniSqlError&) { rollbackBlocked = true; }
                    try { auto guard = buffer.get(last.page); } catch (const MiniSqlError&) { cacheBlocked = true; }
                    if (!rollbackBlocked || !cacheBlocked) throw std::runtime_error("published failure was not quarantined");
                    std::cout << "REOPEN_REQUIRED\n";return 0;
                }
            } else buffer.commitWriteBatch();
        } else if (mode != "read") throw std::runtime_error("unknown probe mode");
        std::vector<Row> rows;heap.scan(10, schema, [&](RowRef, const Row& row) { rows.push_back(row); });
        std::string first;
        for (const auto& row : rows) {
            const auto id = std::get<std::int32_t>(row[0]);
            if (id == 1) first = boolean ? (std::get<bool>(row[1]) ? "true" : "false") : std::get<std::string>(row[1]);
            else if (row[1] != valueFor(id,true)) throw std::runtime_error("Recovery row value mismatch");
        }
        std::cout << "ROWS=" << rows.size() << ";FIRST=" << first << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
