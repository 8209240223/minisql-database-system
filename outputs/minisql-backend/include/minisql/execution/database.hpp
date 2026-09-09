#pragma once
#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
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
    nlohmann::json indexInspect(const std::string& table, const std::string& index);   // 页级索引结构校验（页类型/height/keyCount/兄弟指针/叶链/根可达）
    ~Database();
    void setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile);
    nlohmann::json configureBuffer(const std::string& action);
    nlohmann::json runCorrelatedSubquery(const nlohmann::json& expression, const nlohmann::json& row);
private:
    nlohmann::json bufferStatus() const;
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
