#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/execution/database.hpp"
#include "minisql/sql/serialization.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace minisql;
using namespace minisql::storage;
using namespace minisql::catalog;

int checks = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
    ++checks;
}
template<class F> bool throwsStorage(F action) {
    try { action(); return false; }
    catch (const MiniSqlError& error) { return error.code() == ErrorCode::Storage; }
}
void setHeader(const std::filesystem::path& path, std::int32_t version, const std::string& detail) {
    auto file = std::make_shared<PageFile>(path);
    BufferPool buffer(file, 2, ReplacementPolicy::LRU);
    HeapStore heap(file, buffer);
    const RowSchema headerSchema{ColumnType::Int, ColumnType::Varchar};
    storage::RowRef target{};
    bool found = false;
    heap.scan(CatalogMetaStore, headerSchema, [&](storage::RowRef ref, const storage::Row&) {
        if (!found) { target = ref; found = true; }
    });
    const Row header{version, detail};
    if (found) (void)heap.replace(CatalogMetaStore, headerSchema, target, header);
    else (void)heap.insert(CatalogMetaStore, headerSchema, header);
    heap.flush();
}
int main() {
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("catalog-migration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "db.pages";

    // --- migrationPlan entry point: covers 1 -> current, all reversible ---
    const auto plan = PersistentCatalog::migrationPlan(1);
    {
        require(plan.size() == sql::CATALOG_SCHEMA_VERSION - 1, "migration plan covers chain 1..current");
        require(std::all_of(plan.begin(), plan.end(), [](const CatalogMigrationStep& step) {
            return step.reversible && step.from + 1 == step.to && !step.preflight.empty() &&
                   !step.action.empty() && !step.recoveryPoint.empty();
        }), "each step reversible, sequential, with preflight/action/recovery point");
        require(plan.back().to == sql::CATALOG_SCHEMA_VERSION, "migration plan ends at current version");
    }

    // --- fresh catalog stamps header at current version ---
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        PersistentCatalog catalog(heap);
        require(catalog.catalogSchemaVersion() == sql::CATALOG_SCHEMA_VERSION, "fresh catalog stamped at current version");
        const auto meta = catalog.catalogMetadata();
        require(meta["migratedFrom"].get<std::uint32_t>() == 0 && meta["recovered"].get<bool>() == false,
            "fresh catalog records no migration");
        const auto def = sql::parse(sql::tokenize("CREATE TABLE t(id INT, name VARCHAR);"))[0];
        const auto id = catalog.create(def);
        (void)heap.insert(id, {ColumnType::Int, ColumnType::Varchar}, {std::int32_t(1), std::string("a")});
        heap.flush();
    }

    // --- reopen at current: no re-migration ---
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        PersistentCatalog catalog(heap);
        require(catalog.catalogSchemaVersion() == sql::CATALOG_SCHEMA_VERSION, "current catalog opens without migration");
        require(catalog.catalogMetadata()["recovered"].get<bool>() == false, "no false recovery flag");
        require(catalog.view().find("t") != nullptr, "table retained at current version");
    }

    // --- older header: migration chain upgrades to current, preserves table ---
    setHeader(path, static_cast<std::int32_t>(sql::CATALOG_SCHEMA_VERSION - 1),
        R"({"producerVersion":1,"migratedFrom":0,"recovered":false,"pendingMigration":0})");
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        PersistentCatalog catalog(heap);
        require(catalog.catalogSchemaVersion() == sql::CATALOG_SCHEMA_VERSION, "older catalog migrated to current");
        const auto meta = catalog.catalogMetadata();
        require(meta["migratedFrom"].get<std::uint32_t>() == sql::CATALOG_SCHEMA_VERSION - 1, "migration records source version");
        require(meta["recovered"].get<bool>() == false, "clean migration is not marked recovered");
        require(catalog.view().find("t") != nullptr, "migration preserves table schema");
    }

    // --- interrupted migration (header set to pendingMigration 5): recovery point completes ---
    setHeader(path, static_cast<std::int32_t>(sql::CATALOG_SCHEMA_VERSION - 1),
        R"({"producerVersion":1,"migratedFrom":4,"recovered":false,"pendingMigration":5})");
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        PersistentCatalog catalog(heap);
        require(catalog.catalogSchemaVersion() == sql::CATALOG_SCHEMA_VERSION, "interrupted migration completes at current");
        require(catalog.catalogMetadata()["recovered"].get<bool>() == true, "interrupted migration marked recovered");
        require(catalog.view().find("t") != nullptr, "recovery preserves table");
    }

    // --- unknown newer schema version: rejected, catalog not modified ---
    setHeader(path, 99, R"({"producerVersion":1,"migratedFrom":0,"recovered":false,"pendingMigration":0})");
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        bool rejected = false;
        try { PersistentCatalog catalog(heap); (void)catalog.view(); }
        catch (const MiniSqlError& error) { rejected = std::string(error.what()).find("Unsupported catalog schema") != std::string::npos; }
        require(rejected, "unknown newer catalog schema rejected with unsupported message");
    }

    // --- malformed header (non-positive version): corruption rejected ---
    setHeader(path, 0, R"({"producerVersion":1})");
    require(throwsStorage([&] {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 2, ReplacementPolicy::LRU);
        HeapStore heap(file, buffer);
        PersistentCatalog catalog(heap);
        (void)catalog.tables();
    }), "non-positive header schemaVersion rejected as corruption");

    // --- restore a valid older header then prove the database opens end-to-end ---
    setHeader(path, static_cast<std::int32_t>(sql::CATALOG_SCHEMA_VERSION - 1),
        R"({"producerVersion":1,"migratedFrom":4,"recovered":false,"pendingMigration":0})");
    {
        execution::Database db(path);
        const auto cat = db.catalog();
        require(cat["schemaVersion"].get<std::uint32_t>() == sql::CATALOG_SCHEMA_VERSION, "database catalog reports current schema version after migration");
        require(cat["tables"].size() == 1 && cat["tables"][0]["name"] == "t", "database opens after migration with table intact");
        const auto result = db.execute("SELECT * FROM t ORDER BY id;");
        require(result["success"].get<bool>() == true, "query succeeds after migration (database openable)");
    }

    std::cout << checks << " catalog migration/version checks passed\nEvidence: " << directory.string() << '\n';
}