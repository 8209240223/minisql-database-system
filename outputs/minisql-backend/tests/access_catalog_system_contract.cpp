#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/storage/buffer_pool.hpp"
#include "minisql/storage/heap.hpp"
#include "minisql/storage/page_file.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

namespace {
std::filesystem::path testPath() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("minisql-access-catalog-" + std::to_string(stamp) + ".pages");
}

void removeArtifacts(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".wal", error);
    std::filesystem::remove(path.string() + ".ckpt", error);
    std::filesystem::remove(path.string() + ".lock", error);
}
}

int main() {
    const auto path = testPath();
    removeArtifacts(path);
    const auto document = nlohmann::json{
        {"users", nlohmann::json::object()},
        {"roles", nlohmann::json::object()},
        {"padding", std::string(9000, 'x')},
    };
    const auto payload = document.dump();
    {
        auto file = std::make_shared<minisql::storage::PageFile>(path);
        minisql::storage::BufferPool buffer(file, 16, minisql::storage::ReplacementPolicy::LRU);
        minisql::storage::HeapStore heap(file, buffer);
        minisql::catalog::PersistentCatalog catalog(heap);
        assert(!catalog.accessCatalogRecord().has_value());
        buffer.beginWriteBatch();
        catalog.storeAccessCatalog(7, payload);
        buffer.commitWriteBatch();
        catalog.reload();
        assert(catalog.accessCatalogRecord().has_value());
        assert(catalog.accessCatalogRecord()->permissionVersion == 7);
        assert(catalog.accessCatalogRecord()->payload == payload);
    }
    {
        auto file = std::make_shared<minisql::storage::PageFile>(path);
        minisql::storage::BufferPool buffer(file, 16, minisql::storage::ReplacementPolicy::LRU);
        minisql::storage::HeapStore heap(file, buffer);
        minisql::catalog::PersistentCatalog catalog(heap);
        assert(catalog.accessCatalogRecord().has_value());
        assert(catalog.accessCatalogRecord()->permissionVersion == 7);
        buffer.beginWriteBatch();
        catalog.storeAccessCatalog(8, payload);
        buffer.rollbackWriteBatch();
        catalog.reload();
        assert(catalog.accessCatalogRecord()->permissionVersion == 7);
    }
    {
        auto file = std::make_shared<minisql::storage::PageFile>(path);
        minisql::storage::BufferPool buffer(file, 16, minisql::storage::ReplacementPolicy::LRU);
        minisql::storage::HeapStore heap(file, buffer);
        minisql::catalog::PersistentCatalog catalog(heap);
        buffer.beginWriteBatch();
        catalog.storeAccessCatalog(8, payload);
        buffer.commitWriteBatch();
    }
    {
        auto file = std::make_shared<minisql::storage::PageFile>(path);
        minisql::storage::BufferPool buffer(file, 16, minisql::storage::ReplacementPolicy::LRU);
        minisql::storage::HeapStore heap(file, buffer);
        minisql::catalog::PersistentCatalog catalog(heap);
        assert(catalog.accessCatalogRecord().has_value());
        assert(catalog.accessCatalogRecord()->permissionVersion == 8);
        assert(catalog.accessCatalogRecord()->payload == payload);
    }
    removeArtifacts(path);
    return 0;
}
