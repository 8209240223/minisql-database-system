#include "minisql/storage/heap.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdlib>

using namespace minisql;
using namespace minisql::storage;
int checks = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
template<class F> void rejects(F action, ErrorCode code) {
    bool caught = false;
    try { action(); } catch (const MiniSqlError& error) { caught = error.code() == code; }
    require(caught, "expected error code");
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read evidence file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    if (argc == 3) {
        const auto path = std::filesystem::path(argv[2]);
        if (!std::filesystem::exists(path)) throw std::runtime_error("probe requires an existing fixture");
        auto file = std::make_shared<PageFile>(path);BufferPool buffer(file, 1, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        const RowSchema schema{ColumnType::Int, ColumnType::Varchar};
        if (std::string(argv[1]) == "--crash") {
            buffer.beginWriteBatch();
            for (std::int32_t i = 10; i < 20; ++i) heap.insert(10, schema, {i, std::string(1500, 'z')});
            heap.flush();
            std::_Exit(73);
        }
        if (std::string(argv[1]) != "--read") throw std::runtime_error("unknown probe mode");
        std::vector<Row> rows;heap.scan(10, schema, [&](RowRef, const Row& row) { rows.push_back(row); });
        require(rows == std::vector<Row>{{std::int32_t(1), std::string("original")}}, "crashed staging absent after reopen");
        std::cout << "Recovered base fixture\n";return 0;
    }
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("batch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "pages.db";
    const RowSchema schema{ColumnType::Int, ColumnType::Varchar};
    RowRef original;
    std::string before;
    {
        auto file = std::make_shared<PageFile>(path);BufferPool buffer(file, 1, ReplacementPolicy::LRU);HeapStore heap(file, buffer);
        original = heap.insert(10, schema, {std::int32_t(1), std::string("original")});heap.flush();
        const auto reusable = buffer.allocate(20);buffer.release(reusable);
        before = bytes(path);
        const auto allocated = file->allocatedPages();
        rejects([&] { buffer.rollbackWriteBatch(); }, ErrorCode::Transaction);
        {
            auto guard = buffer.get(original.page);
            rejects([&] { buffer.beginWriteBatch(); }, ErrorCode::Storage);
        }
        buffer.beginWriteBatch();
        require(file->writeBatchActive(), "batch active");
        rejects([&] { buffer.beginWriteBatch(); }, ErrorCode::Transaction);
        const auto reused = buffer.allocate(30);
        require(reused.id == reusable.id && reused.generation == reusable.generation + 1, "staged free page reuse");
        auto replacement = heap.replace(10, schema, original, {std::int32_t(2), std::string(900, 'r')});
        for (std::int32_t i = 3; i < 83; ++i) heap.insert(10, schema, {i, std::string(100, 'x')});
        heap.flush();
        require(file->pagesFor(10).size() > 1, "staged pages exceed cache");
        require(file->stagedPageCount() > 2, "header and multiple pages staged");
        require(bytes(path) == before, "eviction and flush do not write staged bytes to disk");
        require(std::get<std::int32_t>(heap.read(10, schema, replacement)[0]) == 2, "read own staged replacement");
        std::size_t rows = 0;heap.scan(10, schema, [&](RowRef, const Row&) { ++rows; });
        require(rows == 81, "scan sees staged rows");
        require(buffer.stats().stagedPageWrites > 0 && buffer.stats().stagedPageReads > 0, "staged IO separately counted");
        {
            auto guard = buffer.get(replacement.page);
            rejects([&] { buffer.rollbackWriteBatch(); }, ErrorCode::Storage);
            require(file->writeBatchActive(), "pinned rollback preserves active batch");
        }
        buffer.rollbackWriteBatch();
        require(!file->writeBatchActive() && file->stagedPageCount() == 0, "rollback clears staged pages");
        require(buffer.size() == 0, "rollback discards cache");
        require(file->allocatedPages() == allocated && file->pagesFor(30).empty(), "rollback restores owner map");
        require(heap.read(10, schema, original) == Row({std::int32_t(1), std::string("original")}), "old row restored");
        heap.flush();require(bytes(path) == before, "later flush cannot resurrect rolled back pages");
        buffer.beginWriteBatch();
        buffer.release(original.page);
        require(file->pagesFor(10).empty(), "release visible within batch");
        buffer.rollbackWriteBatch();
        require(heap.read(10, schema, original) == Row({std::int32_t(1), std::string("original")}), "release rollback restores original generation");
        buffer.beginWriteBatch();
        heap.insert(10, schema, {std::int32_t(99), std::string("uncommitted")});heap.flush();
    }
    require(bytes(path) == before, "abandoned batch leaves disk unchanged");
    {
        auto file = std::make_shared<PageFile>(path);BufferPool buffer(file, 1, ReplacementPolicy::FIFO);HeapStore heap(file, buffer);
        require(heap.read(10, schema, original) == Row({std::int32_t(1), std::string("original")}), "reopen sees committed base only");
        std::size_t rows = 0;heap.scan(10, schema, [&](RowRef, const Row&) { ++rows; });
        require(rows == 1, "reopen excludes all abandoned rows");
        buffer.beginWriteBatch();
        require(file->stagedPageCount() == 0, "empty batch has no writes");
        buffer.rollbackWriteBatch();
    }
    std::cout << checks << " write-batch checks passed\nEvidence: " << directory.string() << '\n';
}
