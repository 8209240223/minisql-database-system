#pragma once
#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/sql/planner.hpp"
#include "minisql/storage/bplus_tree.hpp"

namespace minisql::execution {
class Database {
public:
    explicit Database(const std::filesystem::path& path, std::size_t frames = 64,
                      storage::PageFile::CommitObserver observer = {});
    nlohmann::json execute(const std::string& sql, bool optimize = true);
    nlohmann::json executeScript(const std::string& sql, bool optimize = true);
    const char* transactionState() const;
    nlohmann::json compile(const std::string& sql) const;
    nlohmann::json diagnostics(const std::string& sql) const;
    nlohmann::json catalog();
    nlohmann::json statistics();
    nlohmann::json checkpoint();
    ~Database();
    void setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile);
    nlohmann::json configureBuffer(const std::string& action);
    nlohmann::json runCorrelatedSubquery(const nlohmann::json& expression, const nlohmann::json& row);
    // X09 4.x: 相关子查询去相关后，右子计划以 Parameter 节点引用外层列；Apply/SemiJoin
    // 执行器在运行右子计划前把外层行按 paramBinding 写入该参数环境，evaluate() 的
    // Parameter 分支读取之。仅在语句执行期内有效（每次 runStatement 复位）。
    nlohmann::json correlationParam(std::size_t paramId) const;
private:
    std::vector<nlohmann::json> correlationParams_;
    nlohmann::json bufferStatus() const;
    std::shared_ptr<storage::PageFile> file_;
    storage::BufferPool buffer_;
    mutable storage::HeapStore heap_;
    catalog::PersistentCatalog catalog_;
    bool unavailable_ = false;
    enum class TransactionState { Idle, Active, Aborted };
    TransactionState transaction_ = TransactionState::Idle;
    void rollbackBatch();
    nlohmann::json executionFailure(const MiniSqlError& error, nlohmann::json results);
    void requireAvailable() const;
    void checkCancelled() const;
    nlohmann::json runStatement(const sql::LogicalPlan& plan);
    nlohmann::json run(const sql::LogicalPlan& plan);
    nlohmann::json runNode(const sql::LogicalPlan& plan);
    // X09 3.5: 相关子查询按 subquerySql 缓存已解析 AST，执行时以 by-value 参数
    // 绑定替换外层列（不再逐行文本重解析）。值会在 run 时以当前 catalog 重新编译。
    std::unordered_map<std::string, std::vector<sql::Statement>> correlatedAstCache_;
    // X09 3.4: 相关子查询「保守执行优化」——等值/确定性相关的 EXISTS/IN/标量按绑定
    // 参数分组，对每个不同参数物化子查询一次（collection 语义半连接），避免重复执行。
    // 以 (subquerySql|scope) 为形缓存外层列引用，以 (shape|绑定值) 缓存结果行；
    // 缓存生命周期仅在单条语句内（runStatement/EXPLAIN ANALYZE 入口清空）。
    std::unordered_map<std::string, std::vector<std::size_t>> correlatedColumnsCache_;
    std::unordered_map<std::string, nlohmann::json> correlatedRowsCache_;
    // X18 4.2: 供优化器成本估算的表行数统计（真实扫描计数，缺表返回空）。
    // const：可为 `compile()`（const）等只读路径提供估算；扫描仅唤醒缓存、不改逻辑状态，
    // 故 heap_ 标为 mutable。
    std::optional<double> estimatedTableRows(const std::string& tableName) const;
    // X18 4.2-iv: const 的按表列统计扫描（含直方图）。statistics() 复用其输出；
    // compile()/execute() 的优化器列级选择率惰性注入取用该项目。
    std::map<std::string, nlohmann::json> tableStats() const;
    // X18 4.3: 把成本/估计元数据（estimatedRows/estimatedCost/statsSource）落到
    // compile() 顶层 plan / optimizedPlan 的每个节点上，并入 HTTP 契约供工作台展示。
    // 沿用优化器同源的 estimatedTableRows + columnSelectivity；惰性扫描列统计仅在有 Filter 时触发。
    nlohmann::json annotatePlanEstimates(const nlohmann::json& serialized, const std::vector<sql::LogicalPlan>& plans) const;
    void evaluateAutoCheckpoint(std::size_t committedWriteStatements, std::size_t committedDirtyPages);
    std::vector<nlohmann::json>* nodeStats_ = nullptr;
    std::size_t sortMemoryRows_ = 10000;
    std::size_t aggregateMemoryRows_ = 10000;
    std::size_t autoCheckpointWrites_ = 0;
    std::uint64_t autoCheckpointWalBytes_ = 0;
    std::size_t autoCheckpointDirtyPages_ = 0;
    double autoCheckpointDirtyRatio_ = 0.0;
    std::uint64_t autoCheckpointIntervalMs_ = 0;
    std::size_t maxResultRows_ = 0;
    std::size_t pendingAutoCheckpointWrites_ = 0;
    std::uint64_t pendingAutoCheckpointWalBytes_ = 0;
    std::size_t checkpointCount_ = 0;
    std::size_t transactionWriteStatements_ = 0;
    std::chrono::steady_clock::time_point lastCheckpointAt_;
    std::uint64_t lastCheckpointAtMs_ = 0;
    std::uint64_t lastAutoCheckpointAtMs_ = 0;
    std::vector<std::string> lastAutoCheckpointReasons_;
    std::filesystem::path sortTempDirectory_;
    std::filesystem::path cancelFile_;
    std::string sessionId_ = "local";
    std::uint64_t querySequence_ = 0;
    std::uint64_t currentQueryId_ = 0;
    std::uint64_t sortSequence_ = 0;
    std::uint64_t aggregateSequence_ = 0;
    struct RuntimeIndex;
    std::vector<std::unique_ptr<RuntimeIndex>> indexes_;
    void rebuildIndexes(std::uint64_t tableId);
    std::string tableFingerprint(std::uint64_t tableId);
    std::uint64_t indexOwnerId(const std::string& table, const std::string& index) const;
    void clearIndexPages(std::uint64_t owner);
    void persistIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint);
    bool loadIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint);
    void validateUniqueIndexes(std::uint64_t tableId, const storage::Row& row, const std::optional<storage::RowRef>& ignored = std::nullopt);
    std::vector<storage::Row> joinRows(const sql::LogicalPlan& plan);
    nlohmann::json aggregateRows(const sql::LogicalPlan& plan);
    void materializeSubqueries(std::vector<sql::LogicalPlan>& plans);
};
}
