#pragma once
#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include "minisql/catalog/persistent_catalog.hpp"
#include "minisql/execution/executor.hpp"
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
    // 在线一致性快照信息（B4/X26）：空闲时 flush 全部 buffeer 并执行一次检查点，使数据库文件
    // 落入一致状态，并返回当前提交序号、WAL 状态、脏页水位与目录/索引版本，供备份 manifest 记录。
    nlohmann::json snapshotInfo();
    nlohmann::json indexInspect(const std::string& table, const std::string& index);   // 页级索引结构校验（页类型/height/keyCount/兄弟指针/叶链/根可达）
    ~Database();
    void setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile);
    nlohmann::json configureBuffer(const std::string& action);
    nlohmann::json runCorrelatedSubquery(const nlohmann::json& expression, const nlohmann::json& row);
    // 执行器级流式读执行（X25 跨进程协议入口）：解析单条 SELECT，构建 RowStream 并施加
    // 资源预算后逐行产出。onMeta 在首行前回调（含 columns/columnTypes）；onRow 每行回调一次；
    // waitBackpressure 在每行产出后回调（若提供），供跨进程背压/取消确认使用（内部持有 mu_）。
    nlohmann::json streamQuery(const std::string& sql,
                               const std::function<void(nlohmann::json&&)>& onMeta,
                               const std::function<void(Row&&)>& onRow,
                               const std::function<void(std::size_t rowIndex)>& waitBackpressure = {});
private:
    nlohmann::json bufferStatus() const;
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
    // 执行器级流式读路径（X25）：SeqScan/Filter/Project/Sort/Limit/Distinct 组合成 RowStream，
    // 并施加行数/临时文件字节/sort run 数预算，达预算立即停止。经由 MINISQL_EXECUTOR=stream 启用。
    bool streamEligible(const sql::LogicalPlan& plan) const;
    std::unique_ptr<RowStream> buildStream(const sql::LogicalPlan& plan);
    nlohmann::json runStream(const sql::LogicalPlan& plan);
    std::vector<nlohmann::json>* nodeStats_ = nullptr;
    std::size_t sortMemoryRows_ = 10000;
    std::size_t aggregateMemoryRows_ = 10000;
    bool streamReads_ = false;              // MINISQL_EXECUTOR=stream 时启用 RowStream 读路径
    std::uint64_t maxTempFileBytes_ = 0;    // 临时文件字节预算（0=不限）
    std::size_t maxSortRuns_ = 0;           // 排序 run 数预算（0=不限）
    std::size_t maxAggregateStates_ = 0;    // 聚合状态数预算（0=不限）
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
