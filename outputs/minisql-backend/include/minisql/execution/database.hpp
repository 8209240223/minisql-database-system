#pragma once
#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <unordered_map>
#include <vector>
#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/sql/planner.hpp"
#include "minisql/storage/bplus_tree.hpp"
#include "minisql/execution/executor.hpp"

namespace minisql::execution {
class Database {
public:
    explicit Database(const std::filesystem::path& path, std::size_t frames = 64,
                      storage::PageFile::CommitObserver observer = {});
    nlohmann::json execute(const std::string& sql, bool optimize = true);
    nlohmann::json executeScript(const std::string& sql, bool optimize = true);
    nlohmann::json executeStreaming(const std::string& sql,
                                    const std::function<void(const nlohmann::json&)>& emitMeta,
                                    const std::function<bool(const nlohmann::json&)>& emitRow);
    const char* transactionState() const;
    nlohmann::json compile(const std::string& sql) const;
    // 解析并通过当前 Catalog 规范化 SQL 实际访问的基础表对象。
    // 该结果供入口层权限校验使用，别名和派生表作用域不会被当成持久化对象。
    std::vector<std::string> resolveAccessObjects(const std::string& sql) const;
    nlohmann::json diagnostics(const std::string& sql) const;
    nlohmann::json catalog();
    nlohmann::json statistics();
    nlohmann::json checkpoint();
    nlohmann::json createSnapshot(const std::filesystem::path& target);
    nlohmann::json indexInspect(const std::string& table, const std::string& index);   // 页级索引结构校验（页类型/height/keyCount/兄弟指针/叶链/根可达）
    // 将已由入口层校验的权限快照同步到 PersistentCatalog 的保留系统表。
    void synchronizeAccessCatalog(const nlohmann::json& document, std::uint32_t permissionVersion);
    const std::optional<catalog::AccessCatalogRecord>& accessCatalogRecord() const { return catalog_.accessCatalogRecord(); }
    ~Database();
    void setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile);
    nlohmann::json configureBuffer(const std::string& action);
    nlohmann::json runCorrelatedSubquery(const nlohmann::json& expression, const nlohmann::json& row);
private:
    nlohmann::json bufferStatus() const;
    // X18: 实时单遍扫描的表/列/索引统计；ANALYZE 用它生成快照，statistics() 无快照时回退到它。
    nlohmann::json liveTableStatistics();
    // ANALYZE 快照旁路文件（<db>.analyze.json）：读、写路径与失效删除。
    std::filesystem::path analyzeMetadataPath() const;
    std::optional<nlohmann::json> loadAnalyzeMetadata() const;
    void invalidateAnalyzeSnapshot() const;
    void evaluateAutoCheckpoint(std::size_t committedWriteStatements, std::size_t committedDirtyPages);
    void evaluateBackgroundCheckpoint();
    void backgroundSchedulerLoop();
    std::shared_ptr<storage::PageFile> file_;
    storage::BufferPool buffer_;
    storage::HeapStore heap_;
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
    std::unique_ptr<RowStream> scanRowStream(const sql::LogicalPlan& plan);
    std::unique_ptr<RowStream> openRowStream(const sql::LogicalPlan& plan);
    // X09 3.5: 相关子查询按 subquerySql 缓存已解析 AST，执行时以 by-value 参数
    // 绑定替换外层列（不再逐行文本重解析）。值会在 run 时以当前 catalog 重新编译。
    std::unordered_map<std::string, std::vector<sql::Statement>> correlatedAstCache_;
    // X09 3.4: 相关子查询「保守执行优化」——等值/确定性相关的 EXISTS/IN/标量按绑定
    // 参数分组，对每个不同参数物化子查询一次（collection 语义半连接），避免重复执行。
    // 以 (subquerySql|scope) 为形缓存外层列引用，以 (shape|绑定值) 缓存结果行；
    // 缓存生命周期仅在单条语句内（runStatement/EXPLAIN ANALYZE 入口清空）。
    std::unordered_map<std::string, std::vector<std::size_t>> correlatedColumnsCache_;
    std::unordered_map<std::string, nlohmann::json> correlatedRowsCache_;
    std::vector<nlohmann::json>* nodeStats_ = nullptr;
    std::size_t sortMemoryRows_ = 10000;
    std::size_t aggregateMemoryRows_ = 10000;
    std::size_t distinctMemoryRows_ = 10000;
    std::size_t joinMemoryRows_ = 10000;
    std::size_t queryMemoryBytes_ = 64 * 1024 * 1024;
    std::uint64_t tempDiskBytes_ = 1024ull * 1024ull * 1024ull;
    std::shared_ptr<QueryResourceManager> activeResources_;
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
    struct SavepointState {
        storage::PageFileSavepoint file;
        catalog::PersistentCatalog::Snapshot catalog;
    };
    std::unordered_map<std::string, SavepointState> savepoints_;
    std::chrono::steady_clock::time_point lastCheckpointAt_;
    std::uint64_t lastCheckpointAtMs_ = 0;
    std::uint64_t lastAutoCheckpointAtMs_ = 0;
    std::vector<std::string> lastAutoCheckpointReasons_;
    std::uint64_t catalogVersion_ = 1;   // 目录版本（写入持久化检查点记录）
    std::uint64_t indexVersion_ = 1;     // 索引版本（写入持久化检查点记录）
    mutable std::recursive_mutex mu_;    // 串行化公共入口与后台检查点，避免与语句执行竞争
    std::thread scheduler_;
    std::size_t backgroundCheckpointMs_ = 0;
    std::atomic<bool> schedulerStop_{false};
    std::mutex schedulerMutex_;
    std::condition_variable schedulerCv_;
    std::uint64_t schedulerLastEvaluateMs_ = 0;
    std::uint64_t schedulerLastRunMs_ = 0;
    std::vector<std::string> schedulerDeferredReasons_;
    std::filesystem::path sortTempDirectory_;
    std::filesystem::path cancelFile_;
    std::string sessionId_ = "local";
    std::uint64_t querySequence_ = 0;
    std::uint64_t currentQueryId_ = 0;
    std::uint64_t sortSequence_ = 0;
    std::uint64_t aggregateSequence_ = 0;
    std::uint64_t joinSequence_ = 0;
    struct RuntimeIndex;
    bool pageFileIndexes_ = true;   // 索引主路径引擎：true=页级 PageBPlusTree，false=内存 BPlusTree（MINISQL_INDEX_ENGINE=memory 时关闭）
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
