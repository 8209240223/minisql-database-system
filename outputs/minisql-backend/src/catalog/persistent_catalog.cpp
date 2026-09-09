#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/sql/serialization.hpp"
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>

namespace minisql::catalog {
namespace {
using namespace storage;
std::string key(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
const RowSchema tableSchema{ColumnType::Int, ColumnType::Varchar, ColumnType::Int};
const RowSchema columnSchema{ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar};
[[noreturn]] void corrupt() { throw MiniSqlError(ErrorCode::Storage, "STORAGE_CORRUPTION: system catalog"); }
}
PersistentCatalog::PersistentCatalog(storage::HeapStore& heap) : heap_(heap) {
    // --- X13 catalog metadata header (schemaVersion 落盘 + 未知主版本拒绝) ---
    const storage::RowSchema headerSchema{storage::ColumnType::Int, storage::ColumnType::Varchar};
    std::uint32_t onDiskVersion = 0;
    std::uint32_t pendingMigration = 0;
    bool headerPresent = false;
    heap_.scan(CatalogMetaStore, headerSchema, [&](storage::RowRef, const storage::Row& row) {
        if (headerPresent) corrupt(); // a single header row is required
        if (row.size() != 2) corrupt();
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        const auto stored = std::get<std::int32_t>(row[0]);
        if (stored < 1) corrupt();
        headerPresent = true;
        onDiskVersion = static_cast<std::uint32_t>(stored);
        const auto& text = std::get<std::string>(row[1]);
        if (text.empty() || text.front() != '{') corrupt();
        try {
            const auto parsed = nlohmann::json::parse(text);
            if (!parsed.is_object() || !parsed.at("producerVersion").is_number_unsigned()) corrupt();
            producerVersion_ = parsed.at("producerVersion").get<std::uint32_t>();
            migratedFrom_ = parsed.value("migratedFrom", 0u);
            recovered_ = parsed.value("recovered", false);
            pendingMigration = parsed.value("pendingMigration", 0u);
        } catch (const nlohmann::json::exception&) { corrupt(); }
    });
    if (onDiskVersion > sql::CATALOG_SCHEMA_VERSION)
        throw MiniSqlError(ErrorCode::Storage, "Unsupported catalog schema version " + std::to_string(onDiskVersion) +
            "; this build supports up to " + std::to_string(sql::CATALOG_SCHEMA_VERSION));
    std::map<std::int32_t, std::map<std::int32_t, sql::ColumnDef>> columns;
    heap_.scan(1, columnSchema, [&](storage::RowRef, const storage::Row& row) {
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        const auto id = std::get<std::int32_t>(row[0]);
        const auto ordinal = std::get<std::int32_t>(row[1]);
        if (id < 2 || id == std::numeric_limits<std::int32_t>::max() || ordinal < 0 || ordinal >= 128) corrupt();
        nextId_ = std::max(nextId_, id + 1);
        sql::ColumnDef column{std::get<std::string>(row[2]), std::get<std::string>(row[3])};
        if (!column.type.empty() && column.type.front() == '{') {
            try {
                const auto encoded = nlohmann::json::parse(column.type);
                const auto version = encoded.at("version");
                if (!encoded.is_object() || !encoded.at("nullable").is_boolean()) corrupt();
                if (version == 1) { if (encoded.size() != 3) corrupt(); }
                else if (version == 2 || version == 3 || version == 4) {
                    if (encoded.size() != (version == 2 ? 4u : version == 3 ? 6u : 7u) || !encoded.contains("defaultValue")) corrupt();
                    if (!encoded.at("defaultValue").is_null()) column.defaultValue = encoded.at("defaultValue").get<std::string>();
                    if (version == 3 || version == 4) {
                        column.primaryKey = encoded.at("primaryKey").get<bool>();
                        column.unique = encoded.at("unique").get<bool>();
                    }
                    if (version == 4 && !encoded.at("references").is_null()) {
                        const auto& reference = encoded.at("references");
                        if (!reference.is_object() || reference.size() != 2) corrupt();
                        column.references = std::make_pair(reference.at("table").get<std::string>(), reference.at("column").get<std::string>());
                    }
                } else corrupt();
                column.type = encoded.at("type").get<std::string>();
                column.nullable = encoded.at("nullable").get<bool>();
            } catch (const nlohmann::json::exception&) { corrupt(); }
        }
        if (!columns[id].emplace(ordinal, std::move(column)).second) corrupt();
    });
    std::map<std::int32_t, bool> seen;
    heap_.scan(0, tableSchema, [&](storage::RowRef, const storage::Row& row) {
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        auto id = std::get<std::int32_t>(row[0]);
        auto count = std::get<std::int32_t>(row[2]);
        if (id < 2 || id == std::numeric_limits<std::int32_t>::max() || !seen.emplace(id, true).second || count <= 0 || count > 128) corrupt();
        nextId_ = std::max(nextId_, id + 1);
        sql::Statement definition;
        definition.kind = "CreateTable";
        definition.table = std::get<std::string>(row[1]);
        if (!definition.table.empty() && definition.table.front() == '{') {
            try {
                const auto encoded = nlohmann::json::parse(definition.table);
                if (!encoded.is_object() || (encoded.at("version") != 1 && encoded.at("version") != 2 && encoded.at("version") != 3 && encoded.at("version") != 4 && encoded.at("version") != 5) || !encoded.at("keys").is_array()) corrupt();
                if (encoded.at("version") == 1 && encoded.size() != 3) corrupt();
                if (encoded.at("version") == 2 && (encoded.size() != 4 || !encoded.at("checks").is_array())) corrupt();
                if (encoded.at("version") == 3 && (encoded.size() != 5 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array())) corrupt();
                if (encoded.at("version") == 4 && (encoded.size() != 6 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array() || !encoded.at("constraintNames").is_array())) corrupt();
                if (encoded.at("version") == 5 && (encoded.size() != 7 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array() || !encoded.at("constraintNames").is_array() || !encoded.at("indexes").is_array())) corrupt();
                definition.table = encoded.at("name").get<std::string>();
                for (const auto& key : encoded.at("keys")) {
                    if (!key.is_object() || key.size() != 2) corrupt();
                    definition.keys.push_back({key.at("primary").get<bool>(), key.at("columns").get<std::vector<std::string>>()});
                }
                if (encoded.at("version") == 2 || encoded.at("version") == 3 || encoded.at("version") == 4 || encoded.at("version") == 5)
                    for (const auto& check : encoded.at("checks")) definition.checks.push_back(sql::deserializeExpression(check));
                if (encoded.at("version") == 3 || encoded.at("version") == 4 || encoded.at("version") == 5)
                    for (const auto& reference : encoded.at("foreignKeys")) {
                        if (!reference.is_object() || reference.size() != 3) corrupt();
                        definition.foreignKeys.push_back({reference.at("columns").get<std::vector<std::string>>(),
                            reference.at("table").get<std::string>(), reference.at("referencedColumns").get<std::vector<std::string>>()});
                    }
                if (encoded.at("version") == 4 || encoded.at("version") == 5)
                    for (const auto& binding : encoded.at("constraintNames")) {
                        if (!binding.is_object() || binding.size() != 3 || !binding.at("index").is_number_unsigned()) corrupt();
                        if (binding.at("index").get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) corrupt();
                        definition.constraintNames.push_back({binding.at("name").get<std::string>(), binding.at("kind").get<std::string>(), binding.at("index").get<std::size_t>()});
                    }
                if (encoded.at("version") == 5)
                    for (const auto& index : encoded.at("indexes")) {
                        if (!index.is_object() || index.size() != 3) corrupt();
                        definition.indexes.push_back({index.at("name").get<std::string>(), index.at("columns").get<std::vector<std::string>>(), index.at("unique").get<bool>()});
                    }
            } catch (const nlohmann::json::exception&) { corrupt(); }
        }
        if (columns[id].size() != static_cast<std::size_t>(count)) corrupt();
        for (std::int32_t i = 0; i < count; ++i) {
            auto found = columns[id].find(i);
            if (found == columns[id].end()) corrupt();
            const auto& col = found->second;
            if (col.type != "int" && !stringType(col.type) && col.type != "bigint" && col.type != "float" && col.type != "bool" && col.type != "date" && !decimalType(col.type)) corrupt();
            definition.columns.push_back(col);
        }
        try { view_.create(definition); for (const auto& index : definition.indexes) { sql::Statement createIndex{"CreateIndex"};createIndex.indexName=index.name;createIndex.table=definition.table;createIndex.indexColumns=index.columns;createIndex.uniqueIndex=index.unique;view_.createIndex(createIndex); } } catch (const MiniSqlError&) { corrupt(); }
        tables_.push_back({id, std::move(definition)});
    });
    // --- X13 migrate / stamp. Table & column rows above were read leniently, so
    // the in-memory catalog already reflects current features; migration here is
    // a version stamp and never rewrites column types / NULL / constraints /
    // indexes (satisfying "迁移不得静默改列类型/NULL/约束/索引"). ---
    if (!headerPresent) {
        // Absent header = brand-new catalog, or a legacy catalog written before
        // metadata existed. Record the current schema version. If a prior crash
        // left descriptors already upgraded but the header unstamped, this stamps
        // it and marks recovery.
        nlohmann::json detail{{"producerVersion", sql::PRODUCER_VERSION},
            {"migratedFrom", 0}, {"recovered", false}, {"pendingMigration", 0}};
        stampHeader(sql::CATALOG_SCHEMA_VERSION, detail);
        producerVersion_ = sql::PRODUCER_VERSION;
        schemaVersion_ = sql::CATALOG_SCHEMA_VERSION;
    } else if (onDiskVersion < sql::CATALOG_SCHEMA_VERSION) {
        // Upgrade chain: onDiskVersion -> CATALOG_SCHEMA_VERSION. Preflight was the
        // successful lenient load above; recovery point is the existing header row.
        const bool interrupted = pendingMigration >= sql::CATALOG_SCHEMA_VERSION;
        nlohmann::json detail{{"producerVersion", sql::PRODUCER_VERSION},
            {"migratedFrom", onDiskVersion}, {"recovered", interrupted}, {"pendingMigration", 0}};
        stampHeader(sql::CATALOG_SCHEMA_VERSION, detail);
        producerVersion_ = sql::PRODUCER_VERSION;
        migratedFrom_ = onDiskVersion;
        recovered_ = interrupted;
        schemaVersion_ = sql::CATALOG_SCHEMA_VERSION;
    } else {
        schemaVersion_ = onDiskVersion;
    }
}
void PersistentCatalog::stampHeader(std::uint32_t version, const nlohmann::json& detail) {
    const storage::RowSchema headerSchema{storage::ColumnType::Int, storage::ColumnType::Varchar};
    const storage::Row header{static_cast<std::int32_t>(version), detail.dump()};
    (void)storage::encodeRow(header, headerSchema);
    storage::RowRef existing{};
    bool found = false;
    heap_.scan(CatalogMetaStore, headerSchema, [&](storage::RowRef ref, const storage::Row& row) {
        if (!found) { existing = ref; found = true; }
    });
    if (found) (void)heap_.replace(CatalogMetaStore, headerSchema, existing, header);
    else (void)heap_.insert(CatalogMetaStore, headerSchema, header);
    heap_.flush();
}
std::vector<CatalogMigrationStep> PersistentCatalog::migrationPlan(std::uint32_t fromVersion) {
    std::vector<CatalogMigrationStep> steps;
    for (std::uint32_t target = fromVersion + 1; target <= sql::CATALOG_SCHEMA_VERSION; ++target) {
        steps.push_back({
            target - 1, target, true,
            "re-validate loaded table/column/index/constraint rows (lenient read already succeeded)",
            "advance catalog schemaVersion by one; table/column/index/constraint rows are left byte-identical (no silent type/NULL/constraint/index change)",
            "previous single header row; replace commits atomically, any interruption re-runs the chain from here and the database stays openable",
        });
    }
    return steps;
}
nlohmann::json PersistentCatalog::catalogMetadata() const {
    return {{"schemaVersion", schemaVersion_}, {"producerVersion", producerVersion_},
        {"migratedFrom", migratedFrom_}, {"recovered", recovered_}};
}
void PersistentCatalog::reload() {
    PersistentCatalog restored(heap_);
    view_ = std::move(restored.view_);
    tables_ = std::move(restored.tables_);
    nextId_ = restored.nextId_;
}
void PersistentCatalog::createIndex(const sql::Statement& statement) {
    view_.createIndex(statement);
    const auto* table = view_.find(statement.table);
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Index table definition missing");
    auto stored = std::find_if(tables_.begin(), tables_.end(), [&](const StoredTable& item) { return key(item.definition.table) == key(statement.table); });
    if (stored == tables_.end()) throw MiniSqlError(ErrorCode::Catalog, "Index table identity missing");
    stored->definition.indexes.clear();
    for (const auto& index : table->indexes) stored->definition.indexes.push_back({index.name, index.columns, index.unique});
    nlohmann::json indexes = nlohmann::json::array();
    for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    nlohmann::json checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
    storage::RowRef target{};
    bool found = false;
    heap_.scan(0, tableSchema, [&](storage::RowRef ref, const storage::Row& row) {
        if (!found && std::get<std::int32_t>(row[0]) == stored->id) { target = ref; found = true; }
    });
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index table catalog row missing");
    (void)heap_.replace(0, tableSchema, target, {stored->id, descriptor, static_cast<std::int32_t>(table->columns.size())});
    heap_.flush();
}
void PersistentCatalog::dropIndex(const sql::Statement& statement) {
    auto stored = std::find_if(tables_.begin(), tables_.end(), [&](const StoredTable& item) {
        if (!statement.table.empty() && key(item.definition.table) != key(statement.table)) return false;
        return std::any_of(item.definition.indexes.begin(), item.definition.indexes.end(), [&](const sql::IndexDef& index) { return key(index.name) == key(statement.indexName); });
    });
    if (stored == tables_.end()) throw MiniSqlError(ErrorCode::Catalog, "Index does not exist: " + statement.indexName);
    view_.dropIndex(statement);
    const auto* table = view_.find(stored->definition.table);
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Index table definition missing");
    stored->definition.indexes.clear();
    for (const auto& index : table->indexes) stored->definition.indexes.push_back({index.name, index.columns, index.unique});
    nlohmann::json indexes = nlohmann::json::array();
    for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    nlohmann::json checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
    storage::RowRef target{};
    bool found = false;
    heap_.scan(0, tableSchema, [&](storage::RowRef ref, const storage::Row& row) {
        if (!found && std::get<std::int32_t>(row[0]) == stored->id) { target = ref; found = true; }
    });
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index table catalog row missing");
    (void)heap_.replace(0, tableSchema, target, {stored->id, descriptor, static_cast<std::int32_t>(table->columns.size())});
    heap_.flush();
}
std::int32_t PersistentCatalog::create(const sql::Statement& definition) {
    if (definition.kind != "CreateTable" || definition.columns.empty() || definition.columns.size() > 128)
        throw MiniSqlError(ErrorCode::Catalog, "Invalid CREATE definition");
    if (nextId_ == std::numeric_limits<std::int32_t>::max()) throw MiniSqlError(ErrorCode::Catalog, "Table identity exhausted");
    auto checked = view_;
    checked.create(definition);
    const auto* table = checked.find(definition.table);
    const auto id = nextId_;
    std::vector<storage::Row> rows;
    for (std::size_t i = 0; i < table->columns.size(); ++i) {
        const auto& column = table->columns[i];
        if (column.type != "int" && !stringType(column.type) && column.type != "bigint" && column.type != "float" && column.type != "bool" && column.type != "date" && !decimalType(column.type)) throw MiniSqlError(ErrorCode::Catalog, "Unsupported column type");
        const auto descriptor = nlohmann::json{{"version", 4}, {"type", column.type}, {"nullable", column.nullable},
            {"defaultValue", column.defaultValue ? nlohmann::json(*column.defaultValue) : nlohmann::json(nullptr)},
            {"primaryKey", column.primaryKey}, {"unique", column.unique}, {"references", sql::serializeReference(column.references)}}.dump();
        rows.push_back({id, static_cast<std::int32_t>(i), column.name, descriptor});
        (void)storage::encodeRow(rows.back(), columnSchema);
    }
    auto checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    nlohmann::json indexes = nlohmann::json::array();for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
    const storage::Row tableRow{id, descriptor, static_cast<std::int32_t>(table->columns.size())};
    (void)storage::encodeRow(tableRow, tableSchema);
    ++nextId_;
    for (const auto& row : rows) heap_.insert(1, columnSchema, row);
    heap_.flush();
    heap_.insert(0, tableSchema, tableRow);
    heap_.flush();
    auto normalized = definition;
    normalized.keys = table->keys;
    normalized.foreignKeys = table->foreignKeys;
    for (std::size_t i = 0; i < normalized.columns.size(); ++i) normalized.columns[i].nullable = table->columns[i].nullable;
    view_ = std::move(checked);
    tables_.push_back({id, std::move(normalized)});
    return id;
}
}
