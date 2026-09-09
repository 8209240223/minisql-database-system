#include "minisql/storage/buffer_pool.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace minisql;
using namespace minisql::storage;
namespace {
int checks = 0;
void require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    ++checks;
}
void rejects(const std::function<void()>& action, const char* name) {
    bool rejected = false;
    try { action(); } catch (const MiniSqlError&) { rejected = true; }
    require(rejected, name);
}
}
int main() {
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("buffer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    auto file = std::make_shared<PageFile>(directory / "pages.db");
    const auto a = file->allocate(1), b = file->allocate(1), c = file->allocate(1), d = file->allocate(1);
    for (const auto policy : {ReplacementPolicy::LRU, ReplacementPolicy::FIFO}) {
        BufferPool buffer(file, 3, policy);
        for (const auto ref : {a, b, c, a, d, a}) { auto guard = buffer.get(ref); }
        const std::uint64_t expectedHits = policy == ReplacementPolicy::LRU ? 2 : 1;
        require(buffer.stats().hits == expectedHits, "policy hit count");
        require(buffer.stats().misses == 6 - expectedHits, "policy miss count");
        require(buffer.stats().pageReads == buffer.stats().misses, "buffer page reads");
        require(buffer.evictions().front().page.id == (policy == ReplacementPolicy::LRU ? b.id : a.id), "policy victim");
        require(buffer.size() == 3, "capacity bound");
    }
    BufferPool buffer(file, 1, ReplacementPolicy::LRU);
    SlotRef row{};
    {
        auto guard = buffer.get(a);
        row = guard.insert(std::vector<std::uint8_t>{1, 2, 3});
        require(buffer.pins(a) == 1, "guard pins");
        rejects([&] { buffer.get(b); }, "all frames pinned");
        rejects([&] { buffer.release(a); }, "pinned release");
        rejects([&] { buffer.flushAll(); }, "pinned flush");
        rejects([&] { buffer.setPolicy(ReplacementPolicy::FIFO); }, "pinned policy switch");
        {
            auto other = buffer.get(a);
            require(buffer.pins(a) == 2, "multiple pins");
            other = std::move(guard);
            require(buffer.pins(a) == 1, "move assignment unpins old guard");
            rejects([&] { guard.page(); }, "moved guard rejected");
        }
        require(buffer.pins(a) == 0, "guard destruction releases pin");
    }
    { auto guard = buffer.get(b); }
    require(buffer.stats().pageWrites == 1, "dirty eviction writes once");
    require(buffer.evictions().back().dirty, "dirty eviction logged");
    require(file->read(a).read(row) == std::vector<std::uint8_t>({1, 2, 3}), "dirty page persisted");
    {
        auto guard = buffer.get(a);
        guard.erase(row);
    }
    buffer.flush(a);
    const auto writes = buffer.stats().pageWrites;
    buffer.flush(a);
    require(buffer.stats().pageWrites == writes, "clean flush no write");
    require(file->read(a).liveSlots().empty(), "deletion flushed");
    {
        auto guard = buffer.get(a);
        guard.insert(std::vector<std::uint8_t>{9});
    }
    buffer.release(a);
    require(buffer.size() == 0, "release invalidates dirty cache");
    const auto newA = buffer.allocate(2);
    require(newA.id == a.id && newA.generation != a.generation, "buffer page reuse");
    {
        auto guard = buffer.get(newA);
        require(guard.page().liveSlots().empty(), "reused page contains no old dirty data");
        rejects([&] { buffer.get(a); }, "old page ref rejected on cache hit");
    }
    buffer.setPolicy(ReplacementPolicy::FIFO);
    require(buffer.size() == 0 && buffer.stats().hits == 0 && buffer.stats().misses == 0, "policy switch clears cache and stats");
    rejects([&] { BufferPool invalid(file, 0, ReplacementPolicy::LRU); }, "zero capacity rejected");
    const auto failurePath = directory / "failure.db";
    auto failedFile = std::make_shared<PageFile>(failurePath);
    const auto p = failedFile->allocate(1), q = failedFile->allocate(1);
    BufferPool failed(failedFile, 1, ReplacementPolicy::LRU);
    { auto guard = failed.get(p); guard.insert(std::vector<std::uint8_t>{42}); }
    std::filesystem::resize_file(failurePath, kPageSize);
    rejects([&] { failedFile->read(q); }, "inject short read");
    rejects([&] { failed.get(q); }, "unhealthy file rejected before lookup");
    require(failed.evictions().empty(), "no eviction attempted on rejected lookup");
    rejects([&] { failed.allocate(1); }, "dirty eviction failure returned");
    require(failed.size() == 1 && failed.evictions().size() == 1, "failed eviction retains frame and records attempt");
    require(failed.evictions().back().writeBack == "failed", "failed writeback recorded");
    require(failed.stats().pageWrites == 0 && failed.stats().ioErrors == 1, "failed flush counters");
    rejects([&] { failed.flushAll(); }, "dirty flag retained after failure");
    auto readFile = std::make_shared<PageFile>(directory / "read-failure.db");
    const auto missing = readFile->allocate(1);
    BufferPool readBuffer(readFile, 1, ReplacementPolicy::FIFO);
    std::filesystem::resize_file(directory / "read-failure.db", kPageSize);
    rejects([&] { readBuffer.get(missing); }, "short read rejected through buffer");
    require(readBuffer.stats().ioErrors == 1 && readBuffer.stats().pageReads == 0 && readBuffer.stats().misses == 0, "failed read counters");
    std::cout << checks << " buffer contract checks passed\nEvidence: " << directory.string() << '\n';
}
