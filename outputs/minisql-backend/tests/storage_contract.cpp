#include "minisql/storage/page_file.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace minisql;
using namespace minisql::storage;
namespace {
int checks = 0;
void require(bool value, const char* name) {
    if (!value) throw std::runtime_error(name);
    ++checks;
}
void rejects(const std::function<void()>& operation, const char* name) {
    bool rejected = false;
    try { operation(); } catch (const MiniSqlError& e) { rejected = e.code() == ErrorCode::Storage; }
    require(rejected, name);
}
}
int main() {
    SlottedPage page(1, 42);
    const std::vector<std::uint8_t> a(100, 11), b(200, 22), c(150, 33);
    auto first = page.insert(a);
    auto second = page.insert(b);
    require(page.read(first) == a, "insert/read");
    page.erase(first);
    rejects([&] { page.read(first); }, "deleted slot rejected");
    auto replacement = page.insert(c);
    require(replacement.slot == first.slot && replacement.generation > first.generation, "slot generation increments");
    rejects([&] { page.read(first); }, "stale slot after reuse rejected");
    require(page.read(second) == b, "compaction preserves other record");
    SlottedPage restored(page.serialize());
    require(restored.read(replacement) == c, "serialization round trip");
    require(restored.liveSlots().size() == 2, "live scan");
    const auto before = restored.serialize();
    rejects([&] { restored.insert(std::vector<std::uint8_t>(4096)); }, "oversized row rejected");
    require(restored.serialize() == before, "failed insert has no effect");
    SlottedPage full(2, 42);
    auto maximum = full.insert(std::vector<std::uint8_t>(4016, 123));
    require(full.freeSpace() == 0, "exact maximum row fits");
    rejects([&] { full.insert(a); }, "full page rejected");
    full.erase(maximum);
    require(full.insert(std::vector<std::uint8_t>(4016)).slot == maximum.slot, "full page slot reusable");
    auto damaged = page.serialize();
    damaged[4000] ^= 1;
    rejects([&] { SlottedPage invalid(damaged); }, "checksum corruption detected");
    damaged = page.serialize();
    writeUnsigned(damaged, 64, 4, 0);
    writeUnsigned(damaged, 4, 4, checksum(damaged));
    rejects([&] { SlottedPage invalid(damaged); }, "invalid slot bounds detected");
    damaged = page.serialize();
    writeUnsigned(damaged, 36, 4, 10000);
    writeUnsigned(damaged, 4, 4, checksum(damaged));
    rejects([&] { SlottedPage invalid(damaged); }, "invalid slot count detected");

    const auto directory = std::filesystem::path("tests/artifacts") /
        ("storage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "pages.db";
    PageRef original{}, survivor{}, reused{};
    SlotRef persistent{};
    {
        PageFile file(path);
        original = file.allocate(42);
        survivor = file.allocate(43);
        require(original.id != survivor.id, "unique active page ids");
        auto diskPage = file.read(survivor);
        persistent = diskPage.insert(b);
        file.write(diskPage);
        file.release(original);
        rejects([&] { file.read(original); }, "freed page inaccessible");
        rejects([&] { file.release(original); }, "double free rejected");
        rejects([&] { file.release({0, 1}); }, "reserved header protected");
        file.flush();
    }
    {
        PageFile file(path);
        require(file.allocatedPages() == 1, "allocation state restored");
        require(file.read(survivor).read(persistent) == b, "record survives reopen");
        reused = file.allocate(44);
        require(reused.id == original.id && reused.generation > original.generation, "free page reused with generation");
        rejects([&] { file.read(original); }, "stale page rejected after reuse");
        rejects([&] { file.write(SlottedPage(reused.id, 999, reused.generation)); }, "owner change rejected");
        auto diskPage = file.read(survivor);
        diskPage.erase(persistent);
        file.write(diskPage);
        file.flush();
    }
    {
        PageFile file(path);
        require(file.read(survivor).liveSlots().empty(), "deletion survives reopen");
        require(file.read(reused).owner() == 44, "owner survives reopen");
    }
    require(std::filesystem::file_size(path) == 3 * kPageSize, "allocation uses full fixed pages");
    const auto truncated = directory / "truncated.db";
    std::filesystem::copy_file(path, truncated);
    std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
    rejects([&] { PageFile invalid(truncated); }, "truncated file rejected");
    const auto corrupt = directory / "corrupt.db";
    std::filesystem::copy_file(path, corrupt);
    {
        std::fstream stream(corrupt, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(20);
        stream.put('\xff');
    }
    rejects([&] { PageFile invalid(corrupt); }, "corrupt header rejected");
    const auto ioFailure = directory / "io-failure.db";
    std::filesystem::copy_file(path, ioFailure);
    {
        PageFile file(ioFailure);
        std::filesystem::resize_file(ioFailure, kPageSize);
        rejects([&] { file.read(survivor); }, "live short read rejected");
        rejects([&] { file.allocate(45); }, "writes disabled after I/O error");
    }
    std::cout << checks << " storage contract checks passed\nEvidence: " << directory.string() << '\n';
}
