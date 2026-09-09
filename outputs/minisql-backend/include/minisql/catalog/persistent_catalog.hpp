#pragma once
#include "minisql/catalog/catalog.hpp"
#include "minisql/storage/heap.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>

namespace minisql::catalog {
struct StoredTable { std::int32_t id; sql::Statement definition; };
// X13 catalog schema versioning.
// The catalog is persisted across two heap relations (0 = tables, 1 = columns)
// plus a dedicated metadata header that records the catalog schemaVersion.
// CatalogMetaStore is a reserved owner id that can never collide with a user
// table (user table ids are int32 hand-outs that start at 2).
inline constexpr std::uint64_t CatalogMetaStore = std::numeric_limits<std::uint64_t>::max();
struct CatalogMigrationStep {
    std::uint32_t from;
    std::uint32_t to;
    bool reversible;
    std::string preflight;     // validation run before the step
    std::string action;        // what the step changes
    std::string recoveryPoint; // point a crashed step resumes from
};
class PersistentCatalog {
public:
    explicit PersistentCatalog(storage::HeapStore& heap);
    const Catalog& view() const { return view_; }
    const std::vector<StoredTable>& tables() const { return tables_; }
    // X13: the on-disk catalog schemaVersion bounding this open instance.
    std::uint32_t catalogSchemaVersion() const { return schemaVersion_; }
    nlohmann::json catalogMetadata() const;
    // X13: inspectable migration entry point for the [fromVersion, current] range.
    static std::vector<CatalogMigrationStep> migrationPlan(std::uint32_t fromVersion);
    std::int32_t create(const sql::Statement& definition);
    void createIndex(const sql::Statement& definition);
    void dropIndex(const sql::Statement& definition);
    void reload();
private:
    void stampHeader(std::uint32_t version, const nlohmann::json& detail);
    storage::HeapStore& heap_;
    Catalog view_;
    std::vector<StoredTable> tables_;
    std::int32_t nextId_ = 2;
    std::uint32_t schemaVersion_ = 0;
    std::uint32_t migratedFrom_ = 0;
    bool recovered_ = false;
    std::uint32_t producerVersion_ = 0;
};
}
