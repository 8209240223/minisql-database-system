#pragma once
#include "minisql/catalog/catalog.hpp"
#include "minisql/storage/heap.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>
#include <optional>

namespace minisql::catalog {
struct StoredTable { std::int32_t id; sql::Statement definition; };
// 一条已持久化的表记录：id 是内部表编号，definition 是原始建表语句。
// 存原始建表语句的好处是重新加载时可以直接复用同一套建表校验逻辑，不必另写反序列化。
// X13 catalog schema versioning.
// X13 目录架构版本化：目录跨两张堆关系持久化（0 = 表目录，1 = 列目录）。
// The catalog is persisted across two heap relations (0 = tables, 1 = columns)
// （承接上文）另有一个专门的元数据头记录目录的 schemaVersion。
// plus a dedicated metadata header that records the catalog schemaVersion.
// 这样打开数据库时可以先读版本号，再决定是否需要迁移。
// CatalogMetaStore is a reserved owner id that can never collide with a user
// CatalogMetaStore 是一个保留的所有者编号，永远不会与用户表冲突。
// table (user table ids are int32 hand-outs that start at 2).
// 因为用户表的编号是 int32 的派发值，从 2 开始。
inline constexpr std::uint64_t CatalogMetaStore = std::numeric_limits<std::uint64_t>::max();
// 取 uint64 最大值当元数据头的所有者编号，这是用户表绝不可能占用的取值。
// 权限目录使用独立的保留堆表所有者，和表目录、列目录共用同一个 PageFile。
// 权限目录使用另一个保留的堆表所有者，与表目录、列目录共用同一个 PageFile。
inline constexpr std::uint64_t AccessCatalogStore = CatalogMetaStore - 1;
// 取最大值减一，保证与 CatalogMetaStore 不冲突。
struct AccessCatalogRecord {
// 权限目录在磁盘上的记录。
    std::uint32_t permissionVersion;
    // permissionVersion 是该权限目录的版本号，用于判断是否需要重新加载。
    std::string payload;
    // payload 是权限目录主体（JSON 文本）。
};
struct CatalogMigrationStep {
// 一次目录迁移步骤的可检查描述。
    std::uint32_t from;
    // from 是起始版本。
    std::uint32_t to;
    // to 是迁移完成后的版本。
    bool reversible;
    // reversible 表示这一步是否可以安全回退。
    std::string preflight;     // validation run before the step
    // preflight 是执行该步骤前要跑的一致性校验。
    std::string action;        // what the step changes
    // action 描述这一步具体改什么。
    std::string recoveryPoint; // point a crashed step resumes from
    // recoveryPoint 是崩溃后可以从哪里继续恢复。
};
class PersistentCatalog {
// PersistentCatalog 把内存目录固化到堆表，并负责打开时重新加载与版本迁移。
public:
    struct Snapshot {
    // Snapshot 是一次完整的目录快照，用于备份、复制与恢复。
        Catalog view;
        // view 是可直接用于查询的内存目录视图。
        std::vector<StoredTable> tables;
        // tables 是全部已持久化的表记录。
        std::int32_t nextId = 2;
        // nextId 是下一个可分配的表编号，默认从 2 开始（0 和 1 留给目录自身）。
        std::uint32_t schemaVersion = 0;
        // schemaVersion 是磁盘上记录的目录架构版本。
        std::uint32_t migratedFrom = 0;
        // migratedFrom 记录本次打开是从哪个版本迁移上来的。
        bool recovered = false;
        // recovered 表示本次打开是否走过了崩溃恢复路径。
        std::uint32_t producerVersion = 0;
        // producerVersion 是写出这些元数据的程序版本。
        std::optional<AccessCatalogRecord> accessCatalog;
        // accessCatalog 是权限目录快照，未启用权限控制时为空。
    };
    // 快照结构结束。
    explicit PersistentCatalog(storage::HeapStore& heap);
    // 构造：绑定一个已经打开的堆存储；构造过程中会把目录读进内存。
    const Catalog& view() const { return view_; }
    // 返回只读的内存目录视图，供 planner 与执行层做列绑定。
    const std::vector<StoredTable>& tables() const { return tables_; }
    // 返回全部已持久化的表记录。
    // 返回最近一次完整写入的权限目录快照；快照不包含任何额外凭据。
    const std::optional<AccessCatalogRecord>& accessCatalogRecord() const { return accessCatalog_; }
    // 返回最近一次完整写入的权限目录快照（该快照不包含任何额外凭据）。
    // 把权限目录以带版本的分片记录写入现有 PageFile 的系统堆表。
    void storeAccessCatalog(std::uint32_t permissionVersion, const std::string& payload);
    // 把权限目录以带版本的分片记录写入现有 PageFile 的系统堆表。
    // X13: the on-disk catalog schemaVersion bounding this open instance.
    std::uint32_t catalogSchemaVersion() const { return schemaVersion_; }
    // 返回本次打开的实例所对应的磁盘目录架构版本。
    nlohmann::json catalogMetadata() const;
    // 返回可直接观测的目录元数据（版本号、表数量、迁移信息等）。
    // X13: inspectable migration entry point for the [fromVersion, current] range.
    static std::vector<CatalogMigrationStep> migrationPlan(std::uint32_t fromVersion);
    // 返回 [fromVersion, 当前版本] 区间内的迁移步骤清单，便于外部检查与演练。
    std::int32_t create(const sql::Statement& definition);
    // 建表：分配表编号并把定义写进目录堆表。
    void createIndex(const sql::Statement& definition);
    // 建索引：把索引定义写进目录。
    void dropIndex(const sql::Statement& definition);
    // 删索引：从目录里摘掉指定索引。
    void reload();
    // 重新从磁盘加载整个目录。
    Snapshot snapshot() const;
    // 导出当前目录的一份完整快照。
    void restore(const Snapshot& snapshot);
    // 用给定快照覆盖当前目录，用于恢复与迁移。
private:
    void stampHeader(std::uint32_t version, const nlohmann::json& detail);
    // 写元数据头：记录版本号与附加信息。
    storage::HeapStore& heap_;
    // 绑定的堆存储引用。
    Catalog view_;
    // 内存中的目录视图。
    std::vector<StoredTable> tables_;
    // 已持久化的表记录。
    std::int32_t nextId_ = 2;
    // 下一个可分配的表编号。
    std::uint32_t schemaVersion_ = 0;
    // 当前磁盘目录架构版本。
    std::uint32_t migratedFrom_ = 0;
    // 本次打开时的迁移来源版本。
    bool recovered_ = false;
    // 是否走过崩溃恢复路径。
    std::uint32_t producerVersion_ = 0;
    // 写出元数据的程序版本。
    std::optional<AccessCatalogRecord> accessCatalog_;
    // 权限目录快照。
};
}
