#pragma once
#include "minisql/catalog/catalog.hpp"
#include "minisql/storage/heap.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>
#include <optional>

namespace minisql::catalog {
struct StoredTable { std::int32_t id; sql::Statement definition; };
// X13 catalog schema versioning.
// The catalog is persisted across two heap relations (0 = tables, 1 = columns)
// plus a dedicated metadata header that records the catalog schemaVersion.
// CatalogMetaStore is a reserved owner id that can never collide with a user
// table (user table ids are int32 hand-outs that start at 2).
inline constexpr std::uint64_t CatalogMetaStore = std::numeric_limits<std::uint64_t>::max();
// 权限目录使用独立的保留堆表所有者，和表目录、列目录共用同一个 PageFile。
inline constexpr std::uint64_t AccessCatalogStore = CatalogMetaStore - 1;
struct AccessCatalogRecord {
    std::uint32_t permissionVersion;
    std::string payload;
};
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
    struct Snapshot {
        Catalog view;
        std::vector<StoredTable> tables;
        std::int32_t nextId = 2;
        std::uint32_t schemaVersion = 0;
        std::uint32_t migratedFrom = 0;
        bool recovered = false;
        std::uint32_t producerVersion = 0;
        std::optional<AccessCatalogRecord> accessCatalog;
    };
    explicit PersistentCatalog(storage::HeapStore& heap);
    const Catalog& view() const { return view_; }
    const std::vector<StoredTable>& tables() const { return tables_; }
    // 返回最近一次完整写入的权限目录快照；快照不包含任何额外凭据。
    const std::optional<AccessCatalogRecord>& accessCatalogRecord() const { return accessCatalog_; }
    // 把权限目录以带版本的分片记录写入现有 PageFile 的系统堆表。
    void storeAccessCatalog(std::uint32_t permissionVersion, const std::string& payload);
    // X13: the on-disk catalog schemaVersion bounding this open instance.
    std::uint32_t catalogSchemaVersion() const { return schemaVersion_; }
    nlohmann::json catalogMetadata() const;
    // X13: inspectable migration entry point for the [fromVersion, current] range.
    static std::vector<CatalogMigrationStep> migrationPlan(std::uint32_t fromVersion);
    std::int32_t create(const sql::Statement& definition);
    void createIndex(const sql::Statement& definition);
    void dropIndex(const sql::Statement& definition);
    void reload();
    Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);
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
    std::optional<AccessCatalogRecord> accessCatalog_;
};
}
