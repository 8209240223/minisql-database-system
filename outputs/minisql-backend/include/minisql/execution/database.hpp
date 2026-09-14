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
// Database 是整个数据库实例的门面：串起存储、目录、优化器与执行器，并对外提供 SQL 级接口。
public:
    explicit Database(const std::filesystem::path& path, std::size_t frames = 64,
    // 打开或创建数据库；frames 是缓冲池的帧数量。
                      storage::PageFile::CommitObserver observer = {});
                      // （承接上一行）observer 是提交观察者，把提交事件转给上层（如审计或测试）。
    nlohmann::json execute(const std::string& sql, bool optimize = true);
    // 执行一条 SQL 并返回完整结果（JSON）。
    nlohmann::json executeScript(const std::string& sql, bool optimize = true);
    // 执行一段包含多条语句的脚本。
    nlohmann::json executeStreaming(const std::string& sql,
    // 流式执行：先回调元数据，再对每一行回调一次。
                                    const std::function<void(const nlohmann::json&)>& emitMeta,
                                    // （承接上一行）emitMeta 只在开始时调用一次。
                                    const std::function<bool(const nlohmann::json&)>& emitRow);
                                    // （承接上一行）emitRow 每行调用一次，回调返回 false 即中止执行并释放资源。
    const char* transactionState() const;
    // 返回当前事务状态（Idle / Active / Aborted），供会话层展示。
    nlohmann::json compile(const std::string& sql) const;
    // 只做编译（解析、语义校验、生成计划）而不执行，用于 EXPLAIN 与测试。
    // 解析并通过当前 Catalog 规范化 SQL 实际访问的基础表对象。
    // 解析并借助当前目录，把 SQL 实际访问的基础表对象名规范化出来。
    // 该结果供入口层权限校验使用，别名和派生表作用域不会被当成持久化对象。
    // （承接上一行）该结果供入口层做权限校验；别名与派生表作用域不会被当成持久化对象。
    std::vector<std::string> resolveAccessObjects(const std::string& sql) const;
    // 返回实际访问的表对象列表。
    nlohmann::json diagnostics(const std::string& sql) const;
    // 生成诊断信息（词法、语法、语义错误的定位），用于错误报告与测试。
    nlohmann::json catalog();
    // 导出目录内容。
    nlohmann::json statistics();
    // 导出统计信息（表与列的行数、索引基数等）。
    nlohmann::json checkpoint();
    // 手动触发一次检查点。
    nlohmann::json createSnapshot(const std::filesystem::path& target);
    // 把当前数据库复制成一份快照目录。
    nlohmann::json indexInspect(const std::string& table, const std::string& index);   // 页级索引结构校验（页类型/height/keyCount/兄弟指针/叶链/根可达）
    // 页级索引结构校验（页类型、height、keyCount、兄弟指针、叶链、根可达）。
    // 将已由入口层校验的权限快照同步到 PersistentCatalog 的保留系统表。
    // 把已经由入口层校验过的权限快照同步到目录的保留系统表。
    void synchronizeAccessCatalog(const nlohmann::json& document, std::uint32_t permissionVersion);
    // （承接上一行）document 是权限目录本体，permissionVersion 是版本号。
    const std::optional<catalog::AccessCatalogRecord>& accessCatalogRecord() const { return catalog_.accessCatalogRecord(); }
    // 返回权限目录在磁盘上的记录，供会话层判断是否需要重新加载。
    ~Database();
    // 析构：停止调度线程、关闭文件、释放资源。
    void setSessionContext(const std::string& sessionId, const std::filesystem::path& cancelFile);
    // 设置会话上下文：会话编号与用于取消查询的标记文件。
    nlohmann::json configureBuffer(const std::string& action);
    // 调整缓冲池配置（扩容或缩容），返回调整后的状态。
    nlohmann::json runCorrelatedSubquery(const nlohmann::json& expression, const nlohmann::json& row);
    // 在给定行上执行一个相关子查询表达式，返回结果。
private:
    nlohmann::json bufferStatus() const;
    // 返回缓冲池状态（帧数、命中率、淘汰记录等）。
    // X18: 实时单遍扫描的表/列/索引统计；ANALYZE 用它生成快照，statistics() 无快照时回退到它。
    nlohmann::json liveTableStatistics();
    // X18：实时单遍扫描得到的表、列、索引统计。
    // ANALYZE 快照旁路文件（<db>.analyze.json）：读、写路径与失效删除。
    // （承接上文）ANALYZE 用它生成快照；没有快照时 statistics() 回退到它。
    std::filesystem::path analyzeMetadataPath() const;
    // ANALYZE 快照旁路文件（<db>.analyze.json）的路径。
    std::optional<nlohmann::json> loadAnalyzeMetadata() const;
    // 读取该快照；文件不存在或损坏时返回空。
    void invalidateAnalyzeSnapshot() const;
    // 让该快照失效（删除文件），写入语句之后调用。
    void evaluateAutoCheckpoint(std::size_t committedWriteStatements, std::size_t committedDirtyPages);
    // 根据已提交的写语句数与脏页数，判断是否需要触发自动检查点。
    void evaluateBackgroundCheckpoint();
    // 判断是否该由后台线程触发检查点。
    void backgroundSchedulerLoop();
    // 后台调度线程的主循环。
    std::shared_ptr<storage::PageFile> file_;
    // 页文件：所有数据与索引都落在它上面。
    storage::BufferPool buffer_;
    // 缓冲池：页面缓存。
    storage::HeapStore heap_;
    // 堆存储：表数据按堆关系组织。
    catalog::PersistentCatalog catalog_;
    // 持久化目录：系统表加用户表定义。
    bool unavailable_ = false;
    // 数据库不可用标志：打开失败后置位，后续调用直接报错。
    enum class TransactionState { Idle, Active, Aborted };
    // 事务状态机：空闲 / 活动中 / 已中止。
    TransactionState transaction_ = TransactionState::Idle;
    // 当前事务状态。
    void rollbackBatch();
    // 回滚当前批次已经做过的修改。
    nlohmann::json executionFailure(const MiniSqlError& error, nlohmann::json results);
    // 把执行失败包装成统一的结果 JSON（含错误类型、位置与原因）。
    void requireAvailable() const;
    // 若数据库不可用则直接报错。
    void checkCancelled() const;
    // 检查是否收到取消请求，收到就抛出取消错误。
    nlohmann::json runStatement(const sql::LogicalPlan& plan);
    // 执行一条语句对应的计划。
    nlohmann::json run(const sql::LogicalPlan& plan);
    // 执行一个计划（递归执行入口）。
    nlohmann::json runNode(const sql::LogicalPlan& plan);
    // 执行单个计划节点并返回其结果。
    std::unique_ptr<RowStream> scanRowStream(const sql::LogicalPlan& plan);
    // 为表扫描构造行流，按需拉取而不必把整表读进内存。
    std::unique_ptr<RowStream> openRowStream(const sql::LogicalPlan& plan);
    // 为任意计划节点打开一个行流。
    // X09 3.5: 相关子查询按 subquerySql 缓存已解析 AST，执行时以 by-value 参数
    // 绑定替换外层列（不再逐行文本重解析）。值会在 run 时以当前 catalog 重新编译。
    // X09 3.5：相关子查询按 subquerySql 缓存已经解析好的 AST。
    std::unordered_map<std::string, std::vector<sql::Statement>> correlatedAstCache_;
    // （承接上文）执行时以按值参数绑定替换外层列，不再逐行重新做文本解析。
    // X09 3.4: 相关子查询「保守执行优化」——等值/确定性相关的 EXISTS/IN/标量按绑定
    // X09 3.4：相关子查询的保守执行优化。
    // 参数分组，对每个不同参数物化子查询一次（collection 语义半连接），避免重复执行。
    // 等值或确定性相关的 EXISTS / IN / 标量按绑定参数分组，每个不同参数只物化一次子查询。
    // 以 (subquerySql|scope) 为形缓存外层列引用，以 (shape|绑定值) 缓存结果行；
    // 以 (subquerySql|scope) 为形状缓存外层列引用。
    // 缓存生命周期仅在单条语句内（runStatement/EXPLAIN ANALYZE 入口清空）。
    // 以 (shape|绑定值) 缓存结果行；缓存生命周期仅在单条语句内，入口处清空。
    std::unordered_map<std::string, std::vector<std::size_t>> correlatedColumnsCache_;
    // 缓存：形状到外层列下标列表的映射。
    std::unordered_map<std::string, nlohmann::json> correlatedRowsCache_;
    // 缓存：形状加绑定值到结果行的映射。
    std::vector<nlohmann::json>* nodeStats_ = nullptr;
    // 指向当前语句的节点统计数组，供 EXPLAIN ANALYZE 使用。
    std::size_t sortMemoryRows_ = 10000;
    // 排序算子的内存行数上限，超过就溢出到磁盘。
    std::size_t aggregateMemoryRows_ = 10000;
    // 聚合算子的内存行数上限。
    std::size_t autoCheckpointWrites_ = 0;
    // 已提交写语句计数，用于触发自动检查点。
    std::uint64_t autoCheckpointWalBytes_ = 0;
    // 自上次检查点以来累积的 WAL 字节数。
    std::size_t autoCheckpointDirtyPages_ = 0;
    // 脏页计数。
    double autoCheckpointDirtyRatio_ = 0.0;
    // 脏页比例阈值。
    std::uint64_t autoCheckpointIntervalMs_ = 0;
    // 自动检查点的时间间隔（毫秒）。
    std::size_t maxResultRows_ = 0;
    // 单条语句最多返回多少行，0 表示不限制。
    std::size_t pendingAutoCheckpointWrites_ = 0;
    // 待处理的自动检查点写语句数。
    std::uint64_t pendingAutoCheckpointWalBytes_ = 0;
    // 待处理的自动检查点 WAL 字节数。
    std::size_t checkpointCount_ = 0;
    // 已经执行过的检查点次数。
    std::size_t transactionWriteStatements_ = 0;
    // 当前事务内已经执行的写语句数。
    struct SavepointState {
    // 保存点状态：同时记录页文件保存点与目录快照。
        storage::PageFileSavepoint file;
        // 页文件侧的保存点。
        catalog::PersistentCatalog::Snapshot catalog;
        // 目录侧的保存点快照。
    };
    // 保存点结构结束。
    std::unordered_map<std::string, SavepointState> savepoints_;
    // 保存点名称到状态的映射。
    std::chrono::steady_clock::time_point lastCheckpointAt_;
    // 上次检查点的单调时钟时间点。
    std::uint64_t lastCheckpointAtMs_ = 0;
    // 上次检查点时间（毫秒）。
    std::uint64_t lastAutoCheckpointAtMs_ = 0;
    // 上次自动检查点时间（毫秒）。
    std::vector<std::string> lastAutoCheckpointReasons_;
    // 最近一次自动检查点被触发的原因列表。
    std::uint64_t catalogVersion_ = 1;   // 目录版本（写入持久化检查点记录）
    // 目录版本号，会被写进持久化检查点记录。
    std::uint64_t indexVersion_ = 1;     // 索引版本（写入持久化检查点记录）
    // 索引版本号，会被写进持久化检查点记录。
    mutable std::recursive_mutex mu_;    // 串行化公共入口与后台检查点，避免与语句执行竞争
    // 递归互斥锁：串行化公共入口与后台检查点，避免与语句执行竞争。
    std::thread scheduler_;
    // 后台检查点调度线程。
    std::size_t backgroundCheckpointMs_ = 0;
    // 后台检查点间隔（毫秒），0 表示关闭。
    std::atomic<bool> schedulerStop_{false};
    // 调度线程停止标志。
    std::mutex schedulerMutex_;
    // 调度线程的互斥锁。
    std::condition_variable schedulerCv_;
    // 调度线程的条件变量，用于等待与唤醒。
    std::uint64_t schedulerLastEvaluateMs_ = 0;
    // 上次评估时间（毫秒）。
    std::uint64_t schedulerLastRunMs_ = 0;
    // 上次真正执行检查点的时间（毫秒）。
    std::vector<std::string> schedulerDeferredReasons_;
    // 调度器推迟检查点的原因列表。
    std::filesystem::path sortTempDirectory_;
    // 排序溢出用的临时文件目录。
    std::filesystem::path cancelFile_;
    // 取消标记文件路径。
    std::string sessionId_ = "local";
    // 会话编号，默认 local。
    std::uint64_t querySequence_ = 0;
    // 查询序号（自增），用于生成查询 ID。
    std::uint64_t currentQueryId_ = 0;
    // 当前查询 ID。
    std::uint64_t sortSequence_ = 0;
    // 排序临时文件名序号。
    std::uint64_t aggregateSequence_ = 0;
    // 聚合临时文件名序号。
    struct RuntimeIndex;
    // 运行期索引句柄的前置声明。
    bool pageFileIndexes_ = true;   // 索引主路径引擎：true=页级 PageBPlusTree，false=内存 BPlusTree（MINISQL_INDEX_ENGINE=memory 时关闭）
    // 索引主路径引擎开关：true 表示页级 PageBPlusTree，false 表示内存 BPlusTree。
    std::vector<std::unique_ptr<RuntimeIndex>> indexes_;
    // 已经打开的运行期索引。
    void rebuildIndexes(std::uint64_t tableId);
    // 重建某张表的全部索引。
    std::string tableFingerprint(std::uint64_t tableId);
    // 计算表指纹，用于判断索引是否仍然匹配当前数据。
    std::uint64_t indexOwnerId(const std::string& table, const std::string& index) const;
    // 计算某个索引在页文件里的所有者编号。
    void clearIndexPages(std::uint64_t owner);
    // 清空属于该所有者的索引页。
    void persistIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint);
    // 把 B+ 树索引页持久化到页文件。
    bool loadIndexPages(storage::BPlusTree& tree, std::uint64_t owner, const std::string& fingerprint);
    // 从页文件加载 B+ 树索引页；指纹不匹配则放弃加载。
    void validateUniqueIndexes(std::uint64_t tableId, const storage::Row& row, const std::optional<storage::RowRef>& ignored = std::nullopt);
    // 校验唯一索引约束；ignored 用于 UPDATE 时忽略被更新的行自身。
    std::vector<storage::Row> joinRows(const sql::LogicalPlan& plan);
    // 执行连接算子，返回连接结果行。
    nlohmann::json aggregateRows(const sql::LogicalPlan& plan);
    // 执行聚合算子，返回聚合结果。
    void materializeSubqueries(std::vector<sql::LogicalPlan>& plans);
    // 把计划里的子查询先具体化（先算结果，再供外层使用）。
};
}
