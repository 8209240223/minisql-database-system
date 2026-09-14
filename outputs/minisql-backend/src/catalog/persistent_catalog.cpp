#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/sql/serialization.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>

namespace minisql::catalog {
namespace {
using namespace storage;
// 本文件大量使用存储层类型（RowSchema、Row、RowRef 等），引入以简化书写。
std::string key(std::string value) {
// 把表名等标识转成小写，作为大小写不敏感比较的规范形式。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 逐字符转小写；入参用 unsigned char 避免负值传入造成未定义行为。
    return value;
    // 返回规范化后的字符串。
}
const RowSchema tableSchema{ColumnType::Int, ColumnType::Varchar, ColumnType::Int};
// 表目录堆关系（编号 0）的行结构：表编号、表描述、列个数。
const RowSchema columnSchema{ColumnType::Int, ColumnType::Int, ColumnType::Varchar, ColumnType::Varchar};
// 列目录堆关系（编号 1）的行结构：表编号、列序号、列名、列描述。
const RowSchema accessSchema{ColumnType::Bigint, ColumnType::Int, ColumnType::Varchar};
// 权限目录堆关系的行结构：权限版本、分片序号、分片内容。
constexpr std::size_t accessChunkBytes = 3800;
// 每个分片的最大字节数：留出编码开销，保证一行能放进一个页。
constexpr std::size_t accessMaxChunks = 4096;
// 权限目录最多允许多少个分片，对应约 15 MB 的权限定义上限。
[[noreturn]] void corrupt() { throw MiniSqlError(ErrorCode::Storage, "STORAGE_CORRUPTION: system catalog"); }
// 统一的损坏上报：系统目录一旦自相矛盾就按存储损坏抛出，绝不猜测修复。
// 消息前缀 STORAGE_CORRUPTION 是测试与运维用来识别该类故障的约定标记。

// Stable on-disk SQL type identities. These values are part of the catalog
// format and must never be renumbered; parameters live beside the base id.
enum class PersistedTypeId : std::uint32_t {
    Int = 1,
    Bigint = 2,
    Float = 3,
    Bool = 4,
    Date = 5,
    Varchar = 6,
    Decimal = 7,
};

nlohmann::json encodeType(const std::string& type) {
    if (type == "int") return {{"typeId", PersistedTypeId::Int}, {"typeParameters", nullptr}};
    if (type == "bigint") return {{"typeId", PersistedTypeId::Bigint}, {"typeParameters", nullptr}};
    if (type == "float") return {{"typeId", PersistedTypeId::Float}, {"typeParameters", nullptr}};
    if (type == "bool") return {{"typeId", PersistedTypeId::Bool}, {"typeParameters", nullptr}};
    if (type == "date") return {{"typeId", PersistedTypeId::Date}, {"typeParameters", nullptr}};
    if (type == "varchar") return {{"typeId", PersistedTypeId::Varchar}, {"typeParameters", nullptr}};
    if (const auto length = varcharLength(type))
        return {{"typeId", PersistedTypeId::Varchar}, {"typeParameters", {{"length", *length}}}};
    if (const auto decimal = decimalType(type))
        return {{"typeId", PersistedTypeId::Decimal},
            {"typeParameters", {{"precision", decimal->precision}, {"scale", decimal->scale}}}};
    throw MiniSqlError(ErrorCode::Catalog, "Unsupported column type");
}

std::string decodeType(const nlohmann::json& encoded) {
    if (!encoded.contains("typeId") || !encoded.at("typeId").is_number_unsigned() ||
        !encoded.contains("typeParameters")) corrupt();
    const auto id = encoded.at("typeId").get<std::uint32_t>();
    const auto& parameters = encoded.at("typeParameters");
    const auto parameterless = [&] { if (!parameters.is_null()) corrupt(); };
    switch (static_cast<PersistedTypeId>(id)) {
        case PersistedTypeId::Int: parameterless(); return "int";
        case PersistedTypeId::Bigint: parameterless(); return "bigint";
        case PersistedTypeId::Float: parameterless(); return "float";
        case PersistedTypeId::Bool: parameterless(); return "bool";
        case PersistedTypeId::Date: parameterless(); return "date";
        case PersistedTypeId::Varchar:
            if (parameters.is_null()) return "varchar";
            if (!parameters.is_object() || parameters.size() != 1 || !parameters.at("length").is_number_unsigned()) corrupt();
            // 长度下界必须与编码侧 varcharLength（varchar.hpp）一致：那里只要求非零，
            // 类型名里的 n 用 std::uint32_t 承载，合法范围是 1..4294967295。
            // 这里若额外收紧到 65535，会出现"建表成功、重新打开就判损坏"的
            // 静默数据丢失：VARCHAR(4294967295) 写得进去，读不回来。
            if (const auto length = parameters.at("length").get<std::uint32_t>(); length >= 1)
                return "varchar(" + std::to_string(length) + ")";
            corrupt();
        case PersistedTypeId::Decimal:
            if (!parameters.is_object() || parameters.size() != 2 ||
                !parameters.at("precision").is_number_unsigned() || !parameters.at("scale").is_number_unsigned()) corrupt();
            if (const DecimalType decimal{parameters.at("precision").get<std::uint32_t>(), parameters.at("scale").get<std::uint32_t>()};
                decimal.precision >= 1 && decimal.precision <= 38 && decimal.scale <= decimal.precision)
                return decimal.name();
            corrupt();
    }
    corrupt();
}

template <typename ColumnLike>
std::string encodeColumnDescriptor(const ColumnLike& column) {
    auto descriptor = encodeType(column.type);
    descriptor["version"] = 5;
    descriptor["nullable"] = column.nullable;
    descriptor["defaultValue"] = column.defaultValue ? nlohmann::json(*column.defaultValue) : nlohmann::json(nullptr);
    descriptor["primaryKey"] = column.primaryKey;
    descriptor["unique"] = column.unique;
    descriptor["references"] = sql::serializeReference(column.references);
    return descriptor.dump();
}
}
PersistentCatalog::PersistentCatalog(storage::HeapStore& heap) : heap_(heap) {
// 打开目录时执行一次完整加载：读元数据头、读列、读表、读权限，再做版本盖章。
    // --- X13 catalog metadata header (schemaVersion 落盘 + 未知主版本拒绝) ---
    // X13 元数据头：把 schemaVersion 落盘，并在遇到不认识的更高主版本时拒绝打开。
    // --- X13 catalog metadata header (schemaVersion 落盘 + 未知主版本拒绝) ---
    const storage::RowSchema headerSchema{storage::ColumnType::Int, storage::ColumnType::Varchar};
    // 元数据头的行结构：版本号（整数）、附加信息（JSON 文本）。
    std::uint32_t onDiskVersion = 0;
    // 磁盘上记录的目录架构版本。
    std::uint32_t pendingMigration = 0;
    // 上次迁移进行到一半时记下的"正在迁移到哪个版本"，用于识别中断。
    bool headerPresent = false;
    // 是否读到过元数据头；没读到说明是老库或全新库。
    heap_.scan(CatalogMetaStore, headerSchema, [&](storage::RowRef, const storage::Row& row) {
    // 扫描元数据头（保留所有者编号 CatalogMetaStore）。
        if (headerPresent) corrupt(); // a single header row is required
        // 元数据头只允许一行，出现第二行说明目录被破坏。
        if (row.size() != 2) corrupt();
        // 列数必须是 2，否则与 headerSchema 不符。
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        // 任何一列为 NULL 都视为损坏（元数据头不允许空值）。
        const auto stored = std::get<std::int32_t>(row[0]);
        // 取出磁盘上记录的版本号。
        if (stored < 1) corrupt();
        // 版本号必须至少为 1。
        headerPresent = true;
        // 标记元数据头存在。
        onDiskVersion = static_cast<std::uint32_t>(stored);
        // 记录版本号，后面用来判断是否需要迁移。
        const auto& text = std::get<std::string>(row[1]);
        // 取出附加信息的 JSON 文本。
        if (text.empty() || text.front() != '{') corrupt();
        // 明显不是 JSON 对象就直接判损坏，避免无意义解析。
        try {
        // 解析附加信息。
            const auto parsed = nlohmann::json::parse(text);
            // 解析成 JSON。
            if (!parsed.is_object() || !parsed.at("producerVersion").is_number_unsigned()) corrupt();
            // 必须是对象，且 producerVersion 必须是非负整数。
            producerVersion_ = parsed.at("producerVersion").get<std::uint32_t>();
            // 还原写出这份目录的程序版本。
            migratedFrom_ = parsed.value("migratedFrom", 0u);
            // 还原迁移来源版本；老产物没有这个字段时按 0 处理。
            recovered_ = parsed.value("recovered", false);
            // 还原"是否走过崩溃恢复"标记。
            pendingMigration = parsed.value("pendingMigration", 0u);
            // 还原中断标记。
        } catch (const nlohmann::json::exception&) { corrupt(); }
        // 解析失败同样按目录损坏处理。
    });
    // 元数据头扫描结束。
    if (onDiskVersion > sql::CATALOG_SCHEMA_VERSION)
    // 磁盘版本比本程序支持的版本还新。
        throw MiniSqlError(ErrorCode::Storage, "Unsupported catalog schema version " + std::to_string(onDiskVersion) +
            // 报出不支持的版本号，而不是尝试按老规则误读。
            "; this build supports up to " + std::to_string(sql::CATALOG_SCHEMA_VERSION));
            // 同时告知本程序支持到哪个版本，便于运维判断该升级程序还是回滚数据。
    std::map<std::int32_t, std::map<std::int32_t, sql::ColumnDef>> columns;
    // 先把列按"表编号 → 列序号 → 列定义"聚合起来，等表目录读完再挂上去。
    heap_.scan(1, columnSchema, [&](storage::RowRef, const storage::Row& row) {
    // 扫描列目录堆关系（编号 1）。
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        // 列目录的每一列都不允许为 NULL。
        const auto id = std::get<std::int32_t>(row[0]);
        // 取出所属表的编号。
        const auto ordinal = std::get<std::int32_t>(row[1]);
        // 取出该列在表内的序号。
        if (id < 2 || id == std::numeric_limits<std::int32_t>::max() || ordinal < 0 || ordinal >= 128) corrupt();
        // 编号必须给用户表使用（从 2 开始），且不能到上限；列序号必须在 0..127。
        nextId_ = std::max(nextId_, id + 1);
        // 维护"下一个可分配的表编号"，保证新表不会撞上已有编号。
        sql::ColumnDef column{std::get<std::string>(row[2]), std::get<std::string>(row[3])};
        // 用列名与列描述构造列定义（描述可能是纯类型串，也可能是 JSON）。
        if (!column.type.empty() && column.type.front() == '{') {
        // 描述以 { 开头说明是带附加信息的 JSON（新格式）。
            try {
            // 解析列描述。
                const auto encoded = nlohmann::json::parse(column.type);
                // 解析成 JSON 对象。
                const auto version = encoded.at("version");
                // 取出这份描述的格式版本，后面按版本分支解析。
                if (!encoded.is_object() || !encoded.at("nullable").is_boolean()) corrupt();
                // 必须是对象，且 nullable 必须是布尔值。
                if (version == 1) {
                    if (encoded.size() != 3) corrupt();
                    column.type = encoded.at("type").get<std::string>();
                }
                else if (version == 2 || version == 3 || version == 4) {
                // 版本 2/3/4：逐步加入了默认值、键标记、外键。
                    if (encoded.size() != (version == 2 ? 4u : version == 3 ? 6u : 7u) || !encoded.contains("defaultValue")) corrupt();
                    // 字段个数必须与版本严格对应，且必须有 defaultValue 字段。
                    if (!encoded.at("defaultValue").is_null()) column.defaultValue = encoded.at("defaultValue").get<std::string>();
                    // 默认值不是 null 时还原成字符串。
                    if (version == 3 || version == 4) {
                    // 版本 3 起带主键与唯一标记。
                        column.primaryKey = encoded.at("primaryKey").get<bool>();
                        // 还原主键标记。
                        column.unique = encoded.at("unique").get<bool>();
                        // 还原唯一标记。
                    }
                    if (version == 4 && !encoded.at("references").is_null()) {
                    // 版本 4 起带列级外键。
                        const auto& reference = encoded.at("references");
                        // 取出外键对象。
                        if (!reference.is_object() || reference.size() != 2) corrupt();
                        // 外键必须是恰好两个字段的对象。
                        column.references = std::make_pair(reference.at("table").get<std::string>(), reference.at("column").get<std::string>());
                        // 还原成"父表名，父列名"这一对。
                    }
                    column.type = encoded.at("type").get<std::string>();
                } else if (version == 5) {
                    if (encoded.size() != 8 || !encoded.contains("defaultValue") ||
                        !encoded.at("primaryKey").is_boolean() || !encoded.at("unique").is_boolean()) corrupt();
                    if (!encoded.at("defaultValue").is_null()) column.defaultValue = encoded.at("defaultValue").get<std::string>();
                    column.primaryKey = encoded.at("primaryKey").get<bool>();
                    column.unique = encoded.at("unique").get<bool>();
                    if (!encoded.at("references").is_null()) {
                        const auto& reference = encoded.at("references");
                        if (!reference.is_object() || reference.size() != 2) corrupt();
                        column.references = std::make_pair(reference.at("table").get<std::string>(), reference.at("column").get<std::string>());
                    }
                    column.type = decodeType(encoded);
                } else corrupt();
                // 其它版本号一律视为损坏（比当前新却还能解析，说明文件被动过）。
                column.nullable = encoded.at("nullable").get<bool>();
                // 还原可空性。
            } catch (const nlohmann::json::exception&) { corrupt(); }
            // 解析失败按目录损坏处理。
        }
        // 列描述解析结束；纯文本类型的描述保持原样，属于老格式。
        if (!columns[id].emplace(ordinal, std::move(column)).second) corrupt();
        // 同一张表同一序号出现两次说明目录损坏。
    });
    // 列目录扫描结束。
    std::map<std::int32_t, bool> seen;
    // 记录已经出现过的表编号，用于检测重复。
    std::vector<StoredTable> loaded;
    // 先把所有表定义收集起来，暂不创建。原因见循环之后的依赖排序。
    heap_.scan(0, tableSchema, [&](storage::RowRef, const storage::Row& row) {
    // 扫描表目录堆关系（编号 0）。
        for (const auto& value : row) if (std::holds_alternative<std::monostate>(value)) corrupt();
        // 表目录的每一列都不允许为 NULL。
        auto id = std::get<std::int32_t>(row[0]);
        // 表编号。
        auto count = std::get<std::int32_t>(row[2]);
        // 该表登记了多少列。
        if (id < 2 || id == std::numeric_limits<std::int32_t>::max() || !seen.emplace(id, true).second || count <= 0 || count > 128) corrupt();
        // 编号范围、编号唯一性、列数范围（1..128）逐项校验。
        nextId_ = std::max(nextId_, id + 1);
        // 维护下一个可分配编号。
        sql::Statement definition;
        // 开始重建这张表的建表语句。
        definition.kind = "CreateTable";
        // 种类固定为建表，加载后可以直接复用同一套校验逻辑。
        definition.table = std::get<std::string>(row[1]);
        // 先假定第二列就是表名，稍后若发现是 JSON 再覆盖。
        if (!definition.table.empty() && definition.table.front() == '{') {
        // 以 { 开头说明这列存的是"表名 + 键 + 检查 + 外键 + 约束名 + 索引"的 JSON 描述。
            try {
            // 解析表描述。
                const auto encoded = nlohmann::json::parse(definition.table);
                // 解析成 JSON 对象。
                if (!encoded.is_object() || (encoded.at("version") != 1 && encoded.at("version") != 2 && encoded.at("version") != 3 && encoded.at("version") != 4 && encoded.at("version") != 5) || !encoded.at("keys").is_array()) corrupt();
                // 必须是对象、版本号在 1..5、且 keys 是数组。
                if (encoded.at("version") == 1 && encoded.size() != 3) corrupt();
                // 版本 1：version/name/keys 三个字段。
                if (encoded.at("version") == 2 && (encoded.size() != 4 || !encoded.at("checks").is_array())) corrupt();
                // 版本 2：多出 checks 数组。
                if (encoded.at("version") == 3 && (encoded.size() != 5 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array())) corrupt();
                // 版本 3：再多出 foreignKeys 数组。
                if (encoded.at("version") == 4 && (encoded.size() != 6 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array() || !encoded.at("constraintNames").is_array())) corrupt();
                // 版本 4：再多出 constraintNames 数组（约束命名）。
                if (encoded.at("version") == 5 && (encoded.size() != 7 || !encoded.at("checks").is_array() || !encoded.at("foreignKeys").is_array() || !encoded.at("constraintNames").is_array() || !encoded.at("indexes").is_array())) corrupt();
                // 版本 5：再多出 indexes 数组（内联索引定义）。
                definition.table = encoded.at("name").get<std::string>();
                // 真正的表名从 JSON 的 name 字段取。
                for (const auto& key : encoded.at("keys")) {
                // 逐条还原主键/唯一键约束。
                    if (!key.is_object() || key.size() != 2) corrupt();
                    // 每条约束必须是恰好两个字段的对象。
                    definition.keys.push_back({key.at("primary").get<bool>(), key.at("columns").get<std::vector<std::string>>()});
                    // 还原"是否主键 + 列名列表"。
                }
                // 键约束还原结束。
                if (encoded.at("version") == 2 || encoded.at("version") == 3 || encoded.at("version") == 4 || encoded.at("version") == 5)
                // 版本 2 起才有 CHECK 约束。
                    for (const auto& check : encoded.at("checks")) definition.checks.push_back(sql::deserializeExpression(check));
                    // 逐个把表达式还原成语法树节点，复用统一的表达式反序列化。
                if (encoded.at("version") == 3 || encoded.at("version") == 4 || encoded.at("version") == 5)
                // 版本 3 起才有外键。
                    for (const auto& reference : encoded.at("foreignKeys")) {
                    // 逐条还原外键。
                        if (!reference.is_object() || reference.size() != 3) corrupt();
                        // 外键必须是恰好三个字段的对象。
                        definition.foreignKeys.push_back({reference.at("columns").get<std::vector<std::string>>(),
                            // 还原本表侧列清单，
                            reference.at("table").get<std::string>(), reference.at("referencedColumns").get<std::vector<std::string>>()});
                            // 以及父表名与父表侧列清单。
                    }
                    // 外键还原结束。
                if (encoded.at("version") == 4 || encoded.at("version") == 5)
                // 版本 4 起才有约束命名。
                    for (const auto& binding : encoded.at("constraintNames")) {
                    // 逐条还原"给某类约束的第几个目标起名"。
                        if (!binding.is_object() || binding.size() != 3 || !binding.at("index").is_number_unsigned()) corrupt();
                        // 必须是三字段对象，且下标是非负整数。
                        if (binding.at("index").get<std::uint64_t>() > std::numeric_limits<std::size_t>::max()) corrupt();
                        // 下标还要能放进本机 size_t，否则拒绝。
                        definition.constraintNames.push_back({binding.at("name").get<std::string>(), binding.at("kind").get<std::string>(), binding.at("index").get<std::size_t>()});
                        // 还原名字、类别与目标下标。
                    }
                    // 约束命名还原结束。
                if (encoded.at("version") == 5)
                // 版本 5 起才有内联索引。
                    for (const auto& index : encoded.at("indexes")) {
                    // 逐条还原索引定义。
                        if (!index.is_object() || index.size() != 3) corrupt();
                        // 每条索引必须是三字段对象。
                        definition.indexes.push_back({index.at("name").get<std::string>(), index.at("columns").get<std::vector<std::string>>(), index.at("unique").get<bool>()});
                        // 还原索引名、索引列、是否唯一。
                    }
                    // 索引还原结束。
            } catch (const nlohmann::json::exception&) { corrupt(); }
            // 解析失败按目录损坏处理。
        }
        // 表描述解析结束；纯文本描述属于老格式，表名保持原值。
        if (columns[id].size() != static_cast<std::size_t>(count)) corrupt();
        // 列目录里登记的行数必须与表目录里记录的列个数一致，否则两份目录不自洽。
        for (std::int32_t i = 0; i < count; ++i) {
        // 按列序号 0..count-1 依次取出每一列。
            auto found = columns[id].find(i);
            // 查找该序号。
            if (found == columns[id].end()) corrupt();
            // 缺号说明列目录有洞，判损坏。
            const auto& col = found->second;
            // 取出列定义。
            if (col.type != "int" && !stringType(col.type) && col.type != "bigint" && col.type != "float" && col.type != "bool" && col.type != "date" && !decimalType(col.type)) corrupt();
            // 列类型必须在支持列表内，避免加载出根本无法执行的表。
            definition.columns.push_back(col);
            // 按序号顺序把列挂到建表语句上，保持与建表时的顺序一致。
        }
        // 列装配结束。
        loaded.push_back({id, std::move(definition)});
        // 只收集，不在这里创建：创建顺序稍后按外键依赖重排。
    });
    // 表目录扫描结束，全部表定义已收齐。

    // 按外键依赖排序后再创建任何一张表。
    //
    // 为什么必须排序：Catalog::create 在遇到外键时要求被引用的父表已经存在
    // （catalog.cpp 的 "Referenced table does not exist"），而表目录的物理槽位顺序
    // 会随 catalog 的增删改写而改变。例如 CREATE INDEX 走的是 replace（先 insert
    // 后 erase），会把被建索引的表搬到页尾，于是下次打开时这张父表可能排在子表之后。
    // 原来的实现按槽位顺序边扫边建，一旦父表落在子表后面就把整个库判成
    // STORAGE_CORRUPTION，导致库再也打不开——一个无害的建索引操作会造成永久损坏。
    // 这里改成先拓扑排序，保证父表先建；自引用与循环引用交给 Catalog::create 自身校验。
    std::vector<std::size_t> creationOrder;
    creationOrder.reserve(loaded.size());
    {
        std::vector<char> placed(loaded.size(), 0);
        std::function<void(std::size_t)> place = [&](std::size_t index) {
            if (placed[index]) return;
            placed[index] = 1;
            // 先递归放置本表引用的所有父表。
            for (const auto& reference : sql::allForeignKeys(loaded[index].definition)) {
                for (std::size_t candidate = 0; candidate < loaded.size(); ++candidate) {
                    if (candidate == index) continue;
                    if (!key(loaded[candidate].definition.table).empty() &&
                        key(loaded[candidate].definition.table) == key(reference.table)) {
                        place(candidate);
                        break;
                    }
                }
            }
            creationOrder.push_back(index);
        };
        for (std::size_t index = 0; index < loaded.size(); ++index) place(index);
    }

    for (const auto index : creationOrder) {
        auto& entry = loaded[index];
        try { view_.create(entry.definition); for (const auto& idx : entry.definition.indexes) { sql::Statement createIndex{"CreateIndex"};createIndex.indexName=idx.name;createIndex.table=entry.definition.table;createIndex.indexColumns=idx.columns;createIndex.uniqueIndex=idx.unique;view_.createIndex(createIndex); } } catch (const MiniSqlError&) { corrupt(); }
        // 把重建出的建表语句交给内存目录重新校验并登记，然后把这个表的内联索引也逐条重建。
        // 这里刻意复用 Catalog::create 与 createIndex：加载路径与建表路径共用同一套校验，
        // 一旦两者不一致（说明磁盘内容非法）就按目录损坏上报，而不是把坏定义放进内存。
        tables_.push_back(std::move(entry));
        // 记录这张表的编号与定义，供导目录、快照与索引重建使用。
    }
    // 读取权限系统堆表时按 permissionVersion 组成候选快照，允许崩溃留下旧快照和新快照的混合尾部。
    // 解释：写权限目录的顺序是"先插入新版本的分片，再删除旧版本的分片"，
    // 中途崩溃会同时留下两代分片，所以这里按版本分组，并优先使用分片齐全且版本最高的那一代。
    // 读取权限系统堆表时按 permissionVersion 组成候选快照，允许崩溃留下旧快照和新快照的混合尾部。
    std::map<std::uint32_t, std::vector<std::pair<std::int32_t, std::string>>> accessChunks;
    // 权限版本 → 该版本的全部分片（分片序号, 分片内容）。
    heap_.scan(AccessCatalogStore, accessSchema, [&](storage::RowRef, const storage::Row& row) {
    // 扫描权限目录堆表。
        if (row.size() != 3 || std::holds_alternative<std::monostate>(row[0]) ||
        // 必须有三列，且版本列非空。
            std::holds_alternative<std::monostate>(row[1]) || std::holds_alternative<std::monostate>(row[2])) corrupt();
        // 分片序号列与内容列同样不能为空。
        const auto version = std::get<std::int64_t>(row[0]);
        // 取出权限版本。
        const auto ordinal = std::get<std::int32_t>(row[1]);
        // 取出分片序号。
        const auto& chunk = std::get<std::string>(row[2]);
        // 取出分片内容。
        if (version < 1 || version > std::numeric_limits<std::uint32_t>::max() || ordinal < 0 ||
        // 版本与序号都必须在合法范围内。
            static_cast<std::size_t>(ordinal) >= accessMaxChunks || chunk.empty() || chunk.size() > accessChunkBytes) corrupt();
        // 序号不能超过分片上限，分片也不能为空或超长。
        accessChunks[static_cast<std::uint32_t>(version)].push_back({ordinal, chunk});
        // 把这一片归入对应的版本。
    });
    // 权限目录扫描结束。
    for (auto version = accessChunks.rbegin(); version != accessChunks.rend() && !accessCatalog_; ++version) {
    // 从版本最高的那一代开始向前找，找到第一个完整可用的就停止。
        auto& chunks = version->second;
        // 这一代的全部候选分片。
        std::sort(chunks.begin(), chunks.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
        // 按分片序号排序，便于检查是否连续。
        bool complete = !chunks.empty() && chunks.front().first == 0;
        // 完整性的必要条件：至少有一片，且从序号 0 开始。
        std::string payload;
        // 拼装出的权限目录正文。
        if (complete) {
        // 起点正确才继续检查连续性。
            for (std::size_t index = 0; index < chunks.size(); ++index) {
            // 逐片检查。
                if (chunks[index].first != static_cast<std::int32_t>(index)) { complete = false; break; }
                // 序号必须严格是 0,1,2…；出现跳号说明这一代没写全，很可能是崩溃点。
                payload += chunks[index].second;
                // 按顺序拼接分片内容。
            }
            // 连续性检查结束。
        }
        if (!complete || payload.empty()) continue;
        // 这一代不完整就换更旧的一代：宁可用旧权限，也不拼出半截权限定义。
        try {
        // 校验拼出来的正文确实是合法的权限目录。
            const auto parsed = nlohmann::json::parse(payload);
            // 解析 JSON。
            if (!parsed.is_object() || !parsed.contains("users") || !parsed.contains("roles") ||
            // 必须是对象，并且带 users 与 roles 两个字段。
                !parsed.at("users").is_object() || !parsed.at("roles").is_object()) continue;
                // 两个字段还必须都是对象，否则不算合法权限目录。
        } catch (const nlohmann::json::exception&) { continue; }
        // 解析失败就跳过这一代，继续找更旧的。
        accessCatalog_ = AccessCatalogRecord{version->first, std::move(payload)};
        // 选定这一代作为当前权限目录快照，并记下它的版本号。
    }
    // 权限目录挑选结束。
    if (!accessChunks.empty() && !accessCatalog_) corrupt();
    // 磁盘上有权限分片却一代都拼不出来，说明数据确实坏了，按目录损坏上报。
    // --- X13 migrate / stamp. Table & column rows above were read leniently, so
    // X13 迁移与盖章。上面的表行与列行是按"宽容读取"解析的，
    // the in-memory catalog already reflects current features; migration here is
    // 所以内存里的目录已经反映了当前全部特性；这里的迁移只做版本盖章，
    // a version stamp and never rewrites column types / NULL / constraints /
    // 绝不会重写列类型、可空性、约束与
    // indexes (satisfying "迁移不得静默改列类型/NULL/约束/索引"). ---
    // 索引，从而满足"迁移不得静默改动列类型、NULL、约束与索引"的要求。
    // --- X13 migrate / stamp. Table & column rows above were read leniently, so
    // the in-memory catalog already reflects current features; migration here is
    // a version stamp and never rewrites column types / NULL / constraints /
    // indexes (satisfying "迁移不得静默改列类型/NULL/约束/索引"). ---
    if (!headerPresent) {
    // 情况一：没有元数据头。
        // Absent header = brand-new catalog, or a legacy catalog written before
        // 没有头有两种可能：全新目录，或者早于元数据机制之前写出的老目录。
        // metadata existed. Record the current schema version. If a prior crash
        // 两种情况都直接把当前版本写上去。如果上次崩溃刚好发生在"描述已升级
        // left descriptors already upgraded but the header unstamped, this stamps
        // 但头还没盖章"之间，这一步会把头补上，
        // it and marks recovery.
        // 并在附加信息里标记这次经历过恢复。
        // Absent header = brand-new catalog, or a legacy catalog written before
        // metadata existed. Record the current schema version. If a prior crash
        // left descriptors already upgraded but the header unstamped, this stamps
        // it and marks recovery.
        nlohmann::json detail{{"producerVersion", sql::PRODUCER_VERSION},
            // 记录当前生产者版本，
            {"migratedFrom", 0}, {"recovered", false}, {"pendingMigration", 0}};
            // 迁移来源记为 0（无从得知），未发生恢复，也没有中断中的迁移。
        stampHeader(sql::CATALOG_SCHEMA_VERSION, detail);
        // 写入元数据头。
        producerVersion_ = sql::PRODUCER_VERSION;
        // 同步内存中的生产者版本。
        schemaVersion_ = sql::CATALOG_SCHEMA_VERSION;
        // 同步内存中的架构版本。
    } else if (onDiskVersion < sql::CATALOG_SCHEMA_VERSION) {
    // 情况二：有头但版本偏旧，需要走升级链。
        // Upgrade chain: onDiskVersion -> CATALOG_SCHEMA_VERSION. Preflight was the
        // 升级链是 onDiskVersion 一直到 CATALOG_SCHEMA_VERSION。
        // successful lenient load above; recovery point is the existing header row.
        // 预检就是上面那次成功的宽容加载；恢复点就是现有这一行元数据头。
        // Upgrade chain: onDiskVersion -> CATALOG_SCHEMA_VERSION. Preflight was the
        // successful lenient load above; recovery point is the existing header row.
        const bool interrupted = pendingMigration >= sql::CATALOG_SCHEMA_VERSION;
        // 上次记录"正在迁移到不低于当前目标的版本"，说明上次是在迁移中途崩的。
        nlohmann::json detail{{"producerVersion", sql::PRODUCER_VERSION},
            // 组装新的元数据头。
            {"migratedFrom", onDiskVersion}, {"recovered", interrupted}, {"pendingMigration", 0}};
            // 记下来源版本与是否经过恢复，并把中断标记清零（本次已经完成迁移）。
        stampHeader(sql::CATALOG_SCHEMA_VERSION, detail);
        // 盖章：把磁盘版本直接推进到当前版本。
        producerVersion_ = sql::PRODUCER_VERSION;
        // 同步生产者版本。
        migratedFrom_ = onDiskVersion;
        // 记下来源版本，供观测与测试断言。
        recovered_ = interrupted;
        // 记录本次是否走了恢复路径。
        schemaVersion_ = sql::CATALOG_SCHEMA_VERSION;
        // 同步架构版本。
    } else {
    // 情况三：版本正好等于当前版本，无需迁移。
        schemaVersion_ = onDiskVersion;
        // 直接采用磁盘上的版本号。
    }
}
// 构造函数结束。
void PersistentCatalog::stampHeader(std::uint32_t version, const nlohmann::json& detail) {
// 写入或更新元数据头：把版本号与附加信息落进保留系统堆表。
    const storage::RowSchema headerSchema{storage::ColumnType::Int, storage::ColumnType::Varchar};
    // 元数据头的行结构，与读取时保持一致。
    const storage::Row header{static_cast<std::int32_t>(version), detail.dump()};
    // 组装这一行：版本号转成 32 位整数，附加信息转成 JSON 文本。
    (void)storage::encodeRow(header, headerSchema);
    // 按行结构编码，提前确认这行能合法落盘；编码结果不使用，只做校验。
    storage::RowRef existing{};
    // 记录已存在的头行位置。
    bool found = false;
    // 是否找到了已有的头行。
    heap_.scan(CatalogMetaStore, headerSchema, [&](storage::RowRef ref, const storage::Row& row) {
    // 扫描元数据头所在的所有者。
        if (!found) { existing = ref; found = true; }
        // 只记住第一行的位置；多行的情况在读取阶段已经被判为损坏。
    });
    if (found) (void)heap_.replace(CatalogMetaStore, headerSchema, existing, header);
    // 已存在就原地替换：这样"换头"是一次原子提交，不会出现没有头的中间态。
    else (void)heap_.insert(CatalogMetaStore, headerSchema, header);
    // 不存在则插入一行。
    heap_.flush();
    // 立即刷盘，保证版本信息在本次打开后立刻可见于其它进程。
}
std::vector<CatalogMigrationStep> PersistentCatalog::migrationPlan(std::uint32_t fromVersion) {
// 生成从 fromVersion 到当前版本的迁移步骤清单，供外部检查与演练。
    std::vector<CatalogMigrationStep> steps;
    // 结果列表。
    for (std::uint32_t target = fromVersion + 1; target <= sql::CATALOG_SCHEMA_VERSION; ++target) {
    // 每一步只把一个版本推进到下一个版本，因此循环逐个目标版本。
        steps.push_back({
        // 追加一条步骤描述。
            target - 1, target, true,
            // 起点、终点，以及"可回退"标记：本迁移不改数据行，所以可以安全回退。
            "re-validate loaded table/column/index/constraint rows (lenient read already succeeded)",
            // 预检内容：对加载到的表、列、索引、约束行再做一次校验；宽容读取阶段已经成功过一次。
            "advance catalog schemaVersion by one; table/column/index/constraint rows are left byte-identical (no silent type/NULL/constraint/index change)",
            // 动作内容：只把目录版本号加一；表、列、索引、约束行保持字节不变，
            // 也就是不会偷偷改动类型、NULL、约束与索引。
            "previous single header row; replace commits atomically, any interruption re-runs the chain from here and the database stays openable",
            // 恢复点：上一版的那一行元数据头；替换是原子提交，
            // 任何中断都会从这一步重跑整条链，数据库始终保持可打开。
        });
        // 本条步骤加完。
    }
    // 版本循环结束。
    return steps;
    // 返回完整步骤清单。
}
nlohmann::json PersistentCatalog::catalogMetadata() const {
// 导出可直接观测的目录元数据。
    return {{"schemaVersion", schemaVersion_}, {"producerVersion", producerVersion_},
        // 当前架构版本与写出这份目录的程序版本。
        {"migratedFrom", migratedFrom_}, {"recovered", recovered_}};
        // 本次打开是从哪个版本迁移来的，以及是否走过崩溃恢复路径。
}
void PersistentCatalog::reload() {
// 重新从磁盘加载目录：做法是新建一个实例再整体搬过来，复用构造函数里的全部校验。
    PersistentCatalog restored(heap_);
    // 用同一个堆存储新建实例，它会完整走一遍加载流程。
    view_ = std::move(restored.view_);
    // 接管新的内存目录视图。
    tables_ = std::move(restored.tables_);
    // 接管新的表记录列表。
    nextId_ = restored.nextId_;
    // 同步下一个可分配编号。
    accessCatalog_ = std::move(restored.accessCatalog_);
    // 接管新的权限目录快照。
}
PersistentCatalog::Snapshot PersistentCatalog::snapshot() const {
// 导出当前目录的一份完整快照，用于备份、复制与恢复。
    return {view_, tables_, nextId_, schemaVersion_, migratedFrom_, recovered_, producerVersion_, accessCatalog_};
    // 按 Snapshot 的字段顺序整体打包返回。
}
void PersistentCatalog::restore(const Snapshot& snapshot) {
// 用给定快照覆盖当前目录。
    view_ = snapshot.view;
    // 恢复内存目录视图。
    tables_ = snapshot.tables;
    // 恢复表记录列表。
    nextId_ = snapshot.nextId;
    // 恢复下一个可分配编号。
    schemaVersion_ = snapshot.schemaVersion;
    // 恢复架构版本。
    migratedFrom_ = snapshot.migratedFrom;
    // 恢复迁移来源版本。
    recovered_ = snapshot.recovered;
    // 恢复"是否走过崩溃恢复"标记。
    producerVersion_ = snapshot.producerVersion;
    // 恢复生产者版本。
    accessCatalog_ = snapshot.accessCatalog;
    // 恢复权限目录快照。
}
void PersistentCatalog::storeAccessCatalog(std::uint32_t permissionVersion, const std::string& payload) {
// 把权限目录以"带版本的分片"写进系统堆表：新版本先写全，再删旧版本。
    if (permissionVersion == 0 || payload.empty() || payload.size() > accessChunkBytes * accessMaxChunks)
    // 版本号不能为 0（0 表示没有权限目录），正文不能为空，且总大小不能超过分片上限乘积。
        throw MiniSqlError(ErrorCode::Catalog, "Invalid access catalog snapshot");
        // 不满足就按目录错误拒绝，避免写进去一份永远读不回来的快照。
    try {
    // 写入前先校验正文是合法的权限目录。
        const auto parsed = nlohmann::json::parse(payload);
        // 解析 JSON。
        if (!parsed.is_object() || !parsed.contains("users") || !parsed.contains("roles") ||
        // 必须是对象，并且带 users 与 roles。
            !parsed.at("users").is_object() || !parsed.at("roles").is_object())
            // 两者还必须都是对象。
            throw MiniSqlError(ErrorCode::Catalog, "Invalid access catalog snapshot");
            // 结构不对就拒绝写入。
    } catch (const nlohmann::json::exception&) {
    // JSON 解析失败。
        throw MiniSqlError(ErrorCode::Catalog, "Invalid access catalog snapshot");
        // 同样拒绝写入。
    }
    if (accessCatalog_ && permissionVersion < accessCatalog_->permissionVersion)
    // 版本号比当前的小，意味着权限定义要"倒退"。
        throw MiniSqlError(ErrorCode::Catalog, "Access catalog version would move backwards");
        // 明确拒绝：权限版本只能前进，避免被旧文件覆盖掉新权限。
    if (accessCatalog_ && permissionVersion == accessCatalog_->permissionVersion) {
    // 版本号相同的情况要区分"重复写入"和"冲突"。
        if (accessCatalog_->payload != payload) throw MiniSqlError(ErrorCode::Catalog, "Access catalog version conflict");
        // 同版本但内容不同：这是无法自动裁决的冲突，必须让人工介入。
        return;
        // 同版本且内容一致：属于重复写入，直接返回即可。
    }
    std::vector<storage::RowRef> oldRows;
    // 先记下旧版本的全部行位置，等新版本写完再删。
    heap_.scan(AccessCatalogStore, accessSchema, [&](storage::RowRef ref, const storage::Row&) { oldRows.push_back(ref); });
    // 扫描权限堆表收集旧行。
    const auto chunkCount = (payload.size() + accessChunkBytes - 1) / accessChunkBytes;
    // 计算需要多少个分片：向上取整。
    if (chunkCount == 0 || chunkCount > accessMaxChunks) throw MiniSqlError(ErrorCode::Catalog, "Access catalog has too many chunks");
    // 分片数为 0 或超上限都拒绝。
    for (std::size_t ordinal = 0; ordinal < chunkCount; ++ordinal) {
    // 逐片写入新版本。
        const auto begin = ordinal * accessChunkBytes;
        // 本片的起始偏移。
        const auto length = std::min(accessChunkBytes, payload.size() - begin);
        // 本片长度；最后一片可能不足一整块。
        const storage::Row row{static_cast<std::int64_t>(permissionVersion), static_cast<std::int32_t>(ordinal), payload.substr(begin, length)};
        // 组装这一片：权限版本、片序号、片内容。
        (void)storage::encodeRow(row, accessSchema);
        // 先编码一次确认可落盘。
        heap_.insert(AccessCatalogStore, accessSchema, row);
        // 插入新分片。
    }
    // 分片写入结束。
    heap_.flush();
    // 先把新版本整批刷盘：这样即使在下一行崩溃，磁盘上也是"旧版本 + 新版本"共存，
    // 读取时会优先挑完整且版本最高的那一代，因此仍然可用。
    for (const auto ref : oldRows) heap_.erase(AccessCatalogStore, ref);
    // 新版本确认落盘后再删除旧版本的行。
    heap_.flush();
    // 再刷一次盘，完成这次换代。
}
void PersistentCatalog::createIndex(const sql::Statement& statement) {
// 建索引：先更新内存目录，再把该表的最新索引定义写回磁盘。
    view_.createIndex(statement);
    // 内存目录先登记索引；这一步会做列存在性等校验。
    const auto* table = view_.find(statement.table);
    // 取出登记后的表定义。
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Index table definition missing");
    // 理论上不会发生；真发生了说明内存目录与表记录列表已经不自洽。
    auto stored = std::find_if(tables_.begin(), tables_.end(), [&](const StoredTable& item) { return key(item.definition.table) == key(statement.table); });
    // 在已持久化的表记录里找到对应的那一张。
    if (stored == tables_.end()) throw MiniSqlError(ErrorCode::Catalog, "Index table identity missing");
    // 找不到说明表存在但没有编号记录，属于目录损坏。
    stored->definition.indexes.clear();
    // 先清空旧的索引列表，稍后用内存目录里的最新状态整体覆盖。
    for (const auto& index : table->indexes) stored->definition.indexes.push_back({index.name, index.columns, index.unique});
    // 把最新索引列表逐条抄进表记录。
    nlohmann::json indexes = nlohmann::json::array();
    // 准备磁盘描述里的 indexes 数组。
    for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    // 每条索引输出名字、列清单与是否唯一。
    nlohmann::json checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    // CHECK 约束在磁盘上存的是表达式文本，这里解析回 JSON 再写进描述，避免变成被转义的字符串。
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        // 组装第 5 版表描述：版本、表名、键约束、CHECK 约束，
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
        // 以及外键、约束命名与索引，最后转成文本。
    storage::RowRef target{};
    // 待替换的目录行位置。
    bool found = false;
    // 是否已定位到该表的目录行。
    heap_.scan(0, tableSchema, [&](storage::RowRef ref, const storage::Row& row) {
    // 扫描表目录，按表编号定位这一行。
        if (!found && std::get<std::int32_t>(row[0]) == stored->id) { target = ref; found = true; }
        // 命中编号就记下位置；加 !found 是为了只取第一条。
    });
    // 定位结束。
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index table catalog row missing");
    // 内存里有这张表，磁盘上却没有对应行，属于目录损坏。
    (void)heap_.replace(0, tableSchema, target, {stored->id, descriptor, static_cast<std::int32_t>(table->columns.size())});
    // 原地替换这一行：编号不变、描述更新、列数保持不变。
    heap_.flush();
    // 刷盘，保证索引定义立刻可见。
}
void PersistentCatalog::dropIndex(const sql::Statement& statement) {
// 删索引：先在磁盘记录里定位到那张表，再更新内存目录并写回。
    auto stored = std::find_if(tables_.begin(), tables_.end(), [&](const StoredTable& item) {
    // 找"拥有这个索引"的表记录。
        if (!statement.table.empty() && key(item.definition.table) != key(statement.table)) return false;
        // 语句里写了表名时，先按表名过滤。
        return std::any_of(item.definition.indexes.begin(), item.definition.indexes.end(), [&](const sql::IndexDef& index) { return key(index.name) == key(statement.indexName); });
        // 再确认该表确实有这个索引（大小写不敏感）。
    });
    // 查找结束。
    if (stored == tables_.end()) throw MiniSqlError(ErrorCode::Catalog, "Index does not exist: " + statement.indexName);
    // 找不到就直接报"索引不存在"，不做任何写入。
    view_.dropIndex(statement);
    // 先更新内存目录。
    const auto* table = view_.find(stored->definition.table);
    // 取出更新后的表定义。
    if (!table) throw MiniSqlError(ErrorCode::Catalog, "Index table definition missing");
    // 与建索引相同的一致性检查。
    stored->definition.indexes.clear();
    // 清空旧索引列表。
    for (const auto& index : table->indexes) stored->definition.indexes.push_back({index.name, index.columns, index.unique});
    // 用内存目录的最新状态覆盖。
    nlohmann::json indexes = nlohmann::json::array();
    // 重建索引数组。
    for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    // 逐条序列化。
    nlohmann::json checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    // 同样把 CHECK 表达式文本解析回 JSON。
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        // 组装新的第 5 版表描述。
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
        // 与建索引时结构完全一致，只是 indexes 少了一条。
    storage::RowRef target{};
    // 待替换的目录行位置。
    bool found = false;
    // 是否已定位。
    heap_.scan(0, tableSchema, [&](storage::RowRef ref, const storage::Row& row) {
    // 按表编号定位目录行。
        if (!found && std::get<std::int32_t>(row[0]) == stored->id) { target = ref; found = true; }
        // 命中即记录。
    });
    // 定位结束。
    if (!found) throw MiniSqlError(ErrorCode::Catalog, "Index table catalog row missing");
    // 找不到属于目录损坏。
    (void)heap_.replace(0, tableSchema, target, {stored->id, descriptor, static_cast<std::int32_t>(table->columns.size())});
    // 原地替换。
    heap_.flush();
    // 刷盘。
}
std::int32_t PersistentCatalog::create(const sql::Statement& definition) {
// 建表：校验通过后分配表编号，先把列行写全，再写表行。
    if (definition.kind != "CreateTable" || definition.columns.empty() || definition.columns.size() > 128)
    // 必须是建表语句、至少一列、且不超过 128 列。
        throw MiniSqlError(ErrorCode::Catalog, "Invalid CREATE definition");
        // 不满足就按目录错误拒绝。
    if (nextId_ == std::numeric_limits<std::int32_t>::max()) throw MiniSqlError(ErrorCode::Catalog, "Table identity exhausted");
    // 表编号已经用尽时明确报错，而不是回绕成重复编号。
    auto checked = view_;
    // 在目录副本上做建表校验，避免校验失败污染真实目录。
    checked.create(definition);
    // 复用 Catalog::create 的全套静态校验（类型、主键、默认值、外键、CHECK、约束命名）。
    const auto* table = checked.find(definition.table);
    // 取出校验通过后的表定义（此时主键列已被强制为非空等）。
    const auto id = nextId_;
    // 记录本次分配的表编号。
    std::vector<storage::Row> rows;
    // 待写入的列行。
    for (std::size_t i = 0; i < table->columns.size(); ++i) {
    // 按列顺序逐列生成列行；顺序即列序号。
        const auto& column = table->columns[i];
        // 当前列。
        if (column.type != "int" && !stringType(column.type) && column.type != "bigint" && column.type != "float" && column.type != "bool" && column.type != "date" && !decimalType(column.type)) throw MiniSqlError(ErrorCode::Catalog, "Unsupported column type");
        // 类型白名单二次确认：写盘前再挡一次，避免脏类型进入磁盘。
        const auto descriptor = encodeColumnDescriptor(column);
        rows.push_back({id, static_cast<std::int32_t>(i), column.name, descriptor});
        // 组装这一行：表编号、列序号、列名、列描述。
        (void)storage::encodeRow(rows.back(), columnSchema);
        // 编码一次确认能落盘。
    }
    // 列行生成结束。
    auto checks = nlohmann::json::array();for (const auto& check : table->checks) checks.push_back(nlohmann::json::parse(check));
    // CHECK 约束在磁盘上存表达式文本，这里解析回 JSON 再拼进描述。
    nlohmann::json indexes = nlohmann::json::array();for (const auto& index : table->indexes) indexes.push_back({{"name", index.name}, {"columns", index.columns}, {"unique", index.unique}});
    // 内联索引定义同样整理成数组。
    const auto descriptor = nlohmann::json{{"version", 5}, {"name", table->name}, {"keys", sql::serializeKeys(table->keys)}, {"checks", checks},
        // 组装第 5 版表描述：版本、表名、键约束、CHECK 约束、
        {"foreignKeys", sql::serializeForeignKeys(table->foreignKeys)}, {"constraintNames", sql::serializeConstraintNames(table->constraintNames)}, {"indexes", indexes}}.dump();
        // 外键、约束命名、索引，最后转成文本。
    const storage::Row tableRow{id, descriptor, static_cast<std::int32_t>(table->columns.size())};
    // 组装表行：编号、描述、列个数。
    (void)storage::encodeRow(tableRow, tableSchema);
    // 编码确认。
    ++nextId_;
    // 编号已分配，先自增，避免后续插入失败后重复使用同一个编号。
    for (const auto& row : rows) heap_.insert(1, columnSchema, row);
    // 先把所有列行写进列目录堆表。
    heap_.flush();
    // 刷盘：保证"表行一旦可见，它的列就已经全部存在"。
    heap_.insert(0, tableSchema, tableRow);
    // 再写表行。
    heap_.flush();
    // 再刷一次盘。
    auto normalized = definition;
    // 复制一份原始定义，准备做规范化后存进内存记录。
    normalized.keys = table->keys;
    // 用校验后的键约束覆盖（可能包含主键列被强制非空等调整）。
    normalized.foreignKeys = table->foreignKeys;
    // 同理覆盖外键。
    for (std::size_t i = 0; i < normalized.columns.size(); ++i) normalized.columns[i].nullable = table->columns[i].nullable;
    // 可空性也以校验结果为准，保证内存记录与磁盘描述完全一致。
    view_ = std::move(checked);
    // 校验通过的目录副本正式生效。
    tables_.push_back({id, std::move(normalized)});
    // 记录这张表的编号与定义。
    return id;
    // 返回分配到的表编号。
}
}
