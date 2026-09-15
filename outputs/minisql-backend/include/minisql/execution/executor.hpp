#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace minisql::execution {

class QueryResourceManager {
public:
    class MemoryReservation {
    public:
        MemoryReservation() = default;
        MemoryReservation(QueryResourceManager* owner, std::size_t bytes);
        MemoryReservation(const MemoryReservation&) = delete;
        MemoryReservation& operator=(const MemoryReservation&) = delete;
        MemoryReservation(MemoryReservation&& other) noexcept;
        MemoryReservation& operator=(MemoryReservation&& other) noexcept;
        ~MemoryReservation();
        void reset();
        std::size_t bytes() const { return bytes_; }
    private:
        QueryResourceManager* owner_ = nullptr;
        std::size_t bytes_ = 0;
    };

    class TempArtifact {
    public:
        TempArtifact(QueryResourceManager& owner, std::filesystem::path path);
        TempArtifact(const TempArtifact&) = delete;
        TempArtifact& operator=(const TempArtifact&) = delete;
        ~TempArtifact();
        void account(std::size_t bytes);
        void cleanup();
        const std::filesystem::path& path() const { return path_; }
    private:
        QueryResourceManager* owner_;
        std::filesystem::path path_;
        std::size_t bytes_ = 0;
        bool cleaned_ = false;
    };

    QueryResourceManager(std::size_t memoryLimitBytes, std::uint64_t tempDiskLimitBytes);
    MemoryReservation reserveMemory(std::size_t bytes);
    std::unique_ptr<TempArtifact> createTempArtifact(const std::filesystem::path& path);
    std::size_t memoryLimitBytes() const { return memoryLimitBytes_; }
    nlohmann::json usage() const;
private:
    friend class MemoryReservation;
    friend class TempArtifact;
    void acquireMemory(std::size_t bytes);
    void releaseMemory(std::size_t bytes) noexcept;
    void acquireDisk(std::size_t bytes);
    void releaseDisk(std::size_t bytes) noexcept;
    void artifactCreated();
    void artifactCleaned() noexcept;
    std::size_t memoryLimitBytes_;
    std::uint64_t tempDiskLimitBytes_;
    std::size_t memoryCurrentBytes_ = 0;
    std::size_t memoryPeakBytes_ = 0;
    std::uint64_t tempDiskCurrentBytes_ = 0;
    std::uint64_t tempDiskPeakBytes_ = 0;
    std::uint64_t spillBytes_ = 0;
    std::size_t spillFiles_ = 0;
    std::size_t tempFilesCleaned_ = 0;
};

class RowStream {
// RowStream 是所有算子的统一读取接口：上层只负责一行一行地取，不关心下面是扫描、连接还是聚合。
public:
// 公开接口，供执行层内部的各个算子实现类继承。
    virtual ~RowStream() = default;
    // 虚析构：通过基类指针删除派生算子时，必须能正确释放派生类持有的资源。
    virtual bool next(nlohmann::json& row) = 0;
    // 取下一行：取到返回 true 并把该行写进 row；已经取完返回 false。
    virtual void cancel() = 0;
    // 请求取消：排序、连接这类长跑算子据此尽快停下并释放资源。
    virtual void close() = 0;
    // 关闭并释放资源；执行结束后必须调用，保证文件句柄与临时文件被回收。
    virtual nlohmann::json resourceUsage() const = 0;
    // 汇报本算子占用的资源（行数、内存、溢出到磁盘的字节数等），供可观测性与结果预算控制使用。
};

using RowTransform = std::function<nlohmann::json(const nlohmann::json&)>;
using RowCompare = std::function<bool(const nlohmann::json&, const nlohmann::json&)>;
using CancelCheck = std::function<void()>;

class MappingRowStream final : public RowStream {
public:
    MappingRowStream(std::unique_ptr<RowStream> child, RowTransform transform,
                     std::optional<nlohmann::json> emptyInputRow = std::nullopt);
    bool next(nlohmann::json& row) override;
    void cancel() override;
    void close() override;
    nlohmann::json resourceUsage() const override;
private:
    std::unique_ptr<RowStream> child_;
    RowTransform transform_;
    std::optional<nlohmann::json> emptyInputRow_;
    std::size_t rows_ = 0;
    bool sawInput_ = false;
    bool emittedEmpty_ = false;
};

class ExternalSortRowStream final : public RowStream {
public:
    ExternalSortRowStream(std::unique_ptr<RowStream> child, RowCompare compare,
                          std::shared_ptr<QueryResourceManager> resources,
                          std::filesystem::path directory, std::string operationId,
                          std::size_t maxRows, CancelCheck checkCancelled = {},
                          std::optional<std::size_t> outputWidth = std::nullopt,
                          std::optional<std::size_t> topN = std::nullopt);
                          // topN：只保留排序结果的前 N 行（Top-N 有界排序）。
                          // 由优化器的 top-n-sort 规则从上方 Limit 下推而来；为空表示需要完整有序结果。
    ~ExternalSortRowStream() override;
    bool next(nlohmann::json& row) override;
    void cancel() override;
    void close() override;
    nlohmann::json resourceUsage() const override;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

class DistinctRowStream final : public RowStream {
public:
    DistinctRowStream(std::unique_ptr<RowStream> sorted,
                      std::shared_ptr<QueryResourceManager> resources);
    bool next(nlohmann::json& row) override;
    void cancel() override;
    void close() override;
    nlohmann::json resourceUsage() const override;
private:
    std::unique_ptr<RowStream> sorted_;
    std::shared_ptr<QueryResourceManager> resources_;
    std::optional<nlohmann::json> previous_;
    std::size_t rows_ = 0;
    std::size_t duplicates_ = 0;
};

using AggregateCombine = std::function<void(nlohmann::json&, const nlohmann::json&)>;
using AggregateFinalize = std::function<nlohmann::json(const nlohmann::json&, const nlohmann::json&)>;

class GroupedAggregateRowStream final : public RowStream {
public:
    GroupedAggregateRowStream(std::unique_ptr<RowStream> sortedRecords,
                              nlohmann::json initialState, AggregateCombine combine,
                              AggregateFinalize finalize,
                              std::shared_ptr<QueryResourceManager> resources);
    bool next(nlohmann::json& row) override;
    void cancel() override;
    void close() override;
    nlohmann::json resourceUsage() const override;
private:
    std::unique_ptr<RowStream> sortedRecords_;
    nlohmann::json initialState_;
    AggregateCombine combine_;
    AggregateFinalize finalize_;
    std::shared_ptr<QueryResourceManager> resources_;
    std::optional<nlohmann::json> pending_;
    std::size_t groups_ = 0;
};

class JoinRowStream final : public RowStream {
public:
    JoinRowStream(std::unique_ptr<RowStream> left, std::unique_ptr<RowStream> right,
                  std::function<bool(const nlohmann::json&)> predicate,
                  std::shared_ptr<QueryResourceManager> resources,
                  std::filesystem::path directory, std::string operationId,
                  std::size_t maxRows, std::size_t rightWidth, bool leftOuter,
                  std::optional<std::pair<std::size_t, std::size_t>> hashKeys,
                  CancelCheck checkCancelled = {});
    ~JoinRowStream() override;
    bool next(nlohmann::json& row) override;
    void cancel() override;
    void close() override;
    nlohmann::json resourceUsage() const override;
private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace minisql::execution
