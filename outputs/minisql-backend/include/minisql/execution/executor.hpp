#pragma once
// 执行器级流式结果接口（X25）。
//
// RowStream 是与存储解耦的「拉取式」行迭代器：上层的 Sort/Limit/Distinct 按需
// 调用 next() 从下游逐行取数，而不是一次性物化整棵子树。行统一表示为 nlohmann::json
// 数组（与 HTTP NDJSON 逐行协议的单元一致）。所有算子把「行转换 / 比较 / 求值」以
// std::function 注入，从而保持本文件仅依赖 nlohmann::json 与标准库，可单独编译验证。
//
// Aggregate/Join 属「显式物化节点」，继续由 database.cpp 复用既有有界外部聚合 /
// HashJoin 逻辑；本接口聚焦单表读路径的流式执行与预算提前停止。
#include "minisql/common/error.hpp"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace minisql::execution {

// 行 = json（数组形式，每个元素为一列）。
using Row = nlohmann::json;

// 资源使用计数：结果行数、临时文件字节、排序 run 数、聚合状态（分组）数。
struct ResourceUsage {
    std::size_t rowsProduced = 0;
    std::uint64_t tempFileBytes = 0;
    std::size_t runCount = 0;
    std::size_t stateCount = 0;
};

// 资源预算：0 表示不限制。
struct RowBudget {
    std::size_t maxRows = 0;
    std::uint64_t maxTempFileBytes = 0;
    std::size_t maxRuns = 0;
    std::size_t maxStates = 0;
};

// 执行器级行流。
class RowStream {
public:
    virtual ~RowStream() = default;

    // 拉取下一行；返回 false 表示流结束。达到预算或取消时抛 MiniSqlError。
    virtual bool next(Row& row) = 0;

    // 请求取消：尽快停止扫描并在下一次 next() 抛取消错误（5002）。
    virtual void cancel() = 0;

    // 释放临时文件、pin、句柄等资源；可重复调用，幂等。
    virtual void close() = 0;

    virtual ResourceUsage resourceUsage() const = 0;
};

// 用「逐项拉取回调」构造的数据源流（由 database 层提供给 heap 扫描 / 索引扫描）。
class CallbackStream final : public RowStream {
public:
    using Producer = std::function<bool(Row&)>;  // 返回 false 表示数据源耗尽
    explicit CallbackStream(Producer producer, std::function<void()> onClose = {})
        : producer_(std::move(producer)), onClose_(std::move(onClose)) {}

    bool next(Row& row) override {
        if (cancelled_) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        if (closed_ || !producer_) return false;
        if (!producer_(row)) { closed_ = true; return false; }
        ++usage_.rowsProduced;
        return true;
    }
    void cancel() override { cancelled_ = true; if (onClose_) onClose_(); }
    void close() override { if (onClose_) onClose_(); closed_ = true; }
    ResourceUsage resourceUsage() const override { return usage_; }

private:
    Producer producer_;
    std::function<void()> onClose_;
    bool cancelled_ = false;
    bool closed_ = false;
    ResourceUsage usage_;
};

// 将已物化的行序列包装为流。
std::unique_ptr<RowStream> materializeStream(std::vector<Row> rows);

// 过滤器：仅透传 predicate(row)==true 的行。
std::unique_ptr<RowStream> filterStream(std::unique_ptr<RowStream> input,
                                        std::function<bool(const Row&)> predicate);

// 投影：把每行用 projection 映射为新行。
std::unique_ptr<RowStream> projectStream(std::unique_ptr<RowStream> input,
                                         std::function<Row(const Row&)> projection);

// 去重：按整行（json 值）去重，保持首次出现顺序。
std::unique_ptr<RowStream> distinctStream(std::unique_ptr<RowStream> input);

// 限量：跳过 offset 行后返回至多 limit 行（limit==0 表示空结果）。
std::unique_ptr<RowStream> limitStream(std::unique_ptr<RowStream> input,
                                       std::size_t offset, std::uint64_t limit);

// 有界外部排序：内存最多保留 runSize 行，其余落盘为 JSONL run，多路归并后逐行产出。
// less 为严格弱序；directory 为 run 目录；operationId 绑定 session/查询/排序序号。
// checkCancelled 在长循环中周期性调用以支持协作取消。
std::unique_ptr<RowStream> sortStream(std::unique_ptr<RowStream> input,
                                      std::function<bool(const Row&, const Row&)> less,
                                      std::size_t runSize,
                                      const std::filesystem::path& directory,
                                      const std::string& operationId,
                                      std::function<void()> checkCancelled = {});

// 预算装饰器：在 next() 产出前检查行数 / 临时文件字节 / run 数 / 状态数，
// 超限即取消 + 关闭下层并抛 5001，实现「达预算立即停止并释放资源」。
std::unique_ptr<RowStream> budgetStream(std::unique_ptr<RowStream> input, RowBudget budget);

}  // namespace minisql::execution