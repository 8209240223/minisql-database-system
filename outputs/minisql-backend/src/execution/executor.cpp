#include "minisql/execution/executor.hpp"
#include "minisql/execution/external_sort.hpp"
#include <algorithm>
#include <fstream>
#include <optional>
#include <queue>
#include <set>

namespace minisql::execution {
namespace {

struct SortRuns {
    std::vector<std::filesystem::path> runs;
    std::vector<std::filesystem::path> metadata;
    std::uint64_t bytes = 0;
    void clear() {
        for (const auto& file : runs) { std::error_code ec; std::filesystem::remove(file, ec); }
        for (const auto& file : metadata) { std::error_code ec; std::filesystem::remove(file, ec); }
        runs.clear(); metadata.clear(); bytes = 0;
    }
    ~SortRuns() { clear(); }
};

void writeRun(const std::vector<Row>& rows, const std::filesystem::path& directory,
              const std::string& prefix, std::size_t index, SortRuns& sink) {
    const auto run = directory / (prefix + std::to_string(index) + ".jsonl");
    std::ofstream output(run, std::ios::binary | std::ios::trunc);
    if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort run");
    std::uint64_t checksum = detail::checksumBytes("");
    std::size_t runRows = 0;
    for (const auto& row : rows) {
        const auto line = row.dump();
        output << line << '\n';
        sink.bytes += line.size() + 1;
        checksum = detail::checksumBytes(line, checksum);
        checksum = detail::checksumBytes("\n", checksum);
        ++runRows;
    }
    if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort run");
    const auto metadataFile = std::filesystem::path(run.string() + ".meta.json");
    std::ofstream metadataOutput(metadataFile, std::ios::binary | std::ios::trunc);
    if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort metadata");
    metadataOutput << nlohmann::json{{"operationId", prefix}, {"runIndex", index}, {"rows", runRows},
        {"checksumAlgorithm", "fnv1a64"}, {"checksum", detail::checksumText(checksum)}}.dump() << '\n';
    if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort metadata");
    sink.runs.push_back(run);
    sink.metadata.push_back(metadataFile);
}

}  // namespace

std::unique_ptr<RowStream> materializeStream(std::vector<Row> rows) {
    auto data = std::make_shared<std::vector<Row>>(std::move(rows));
    auto index = std::make_shared<std::size_t>(0);
    return std::make_unique<CallbackStream>([data, index](Row& row) {
        if (*index >= data->size()) return false;
        row = (*data)[(*index)++];
        return true;
    });
}

namespace {
class FilterStream final : public RowStream {
public:
    FilterStream(std::unique_ptr<RowStream> input, std::function<bool(const Row&)> predicate)
        : input_(std::move(input)), predicate_(std::move(predicate)) {}
    bool next(Row& row) override {
        Row candidate;
        while (input_->next(candidate)) {
            if (predicate_(candidate)) { row = std::move(candidate); ++rows_; return true; }
        }
        return false;
    }
    void cancel() override { input_->cancel(); }
    void close() override { input_->close(); }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.rowsProduced = rows_;
        return usage;
    }
private:
    std::unique_ptr<RowStream> input_;
    std::function<bool(const Row&)> predicate_;
    std::size_t rows_ = 0;
};

class ProjectStream final : public RowStream {
public:
    ProjectStream(std::unique_ptr<RowStream> input, std::function<Row(const Row&)> projection)
        : input_(std::move(input)), projection_(std::move(projection)) {}
    bool next(Row& row) override {
        Row candidate;
        if (!input_->next(candidate)) return false;
        row = projection_(candidate);
        ++rows_;
        return true;
    }
    void cancel() override { input_->cancel(); }
    void close() override { input_->close(); }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.rowsProduced = rows_;
        return usage;
    }
private:
    std::unique_ptr<RowStream> input_;
    std::function<Row(const Row&)> projection_;
    std::size_t rows_ = 0;
};

class DistinctStream final : public RowStream {
public:
    explicit DistinctStream(std::unique_ptr<RowStream> input) : input_(std::move(input)) {}
    bool next(Row& row) override {
        Row candidate;
        while (input_->next(candidate)) {
            if (seen_.insert(candidate).second) { row = std::move(candidate); ++rows_; return true; }
        }
        return false;
    }
    void cancel() override { input_->cancel(); }
    void close() override { input_->close(); }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.rowsProduced = rows_;
        return usage;
    }
private:
    std::unique_ptr<RowStream> input_;
    std::set<Row> seen_;
    std::size_t rows_ = 0;
};

class LimitStream final : public RowStream {
public:
    LimitStream(std::unique_ptr<RowStream> input, std::size_t offset, std::uint64_t limit)
        : input_(std::move(input)), offset_(offset), limit_(limit) {}
    bool next(Row& row) override {
        while (skipped_ < offset_) {
            Row ignored;
            if (!input_->next(ignored)) return false;
            ++skipped_;
        }
        if (limit_ == 0 || produced_ >= limit_) return false;
        if (!input_->next(row)) return false;
        ++produced_; ++rows_;
        return true;
    }
    void cancel() override { input_->cancel(); }
    void close() override { input_->close(); }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.rowsProduced = rows_;
        return usage;
    }
private:
    std::unique_ptr<RowStream> input_;
    std::size_t offset_;
    std::uint64_t limit_;
    std::size_t skipped_ = 0;
    std::uint64_t produced_ = 0;
    std::size_t rows_ = 0;
};

class SortStream final : public RowStream {
public:
    SortStream(std::unique_ptr<RowStream> input, std::function<bool(const Row&, const Row&)> less,
               std::size_t runSize, std::filesystem::path directory, std::string operationId,
               std::function<void()> checkCancelled)
        : input_(std::move(input)), less_(std::move(less)), runSize_(runSize),
          directory_(std::move(directory)), prefix_("sort-" + detail::sortArtifactId(std::move(operationId)) + "-"),
          checkCancelled_(std::move(checkCancelled)) {
        if (runSize_ == 0) throw MiniSqlError(ErrorCode::InvalidArgument, "External sort row budget must be positive");
    }
    bool next(Row& row) override {
        prepare();
        if (mergedIndex_ < merged_.size()) { row = std::move(merged_[mergedIndex_++]); ++rows_; return true; }
        if (queue_ && !queue_->empty()) {
            Cursor cursor = queue_->top();
            queue_->pop();
            row = std::move(cursor.row);
            ++rows_;
            std::string line;
            if (std::getline(inputs_[cursor.run], line) && !line.empty())
                queue_->push({nlohmann::json::parse(line), cursor.run, cursor.sequence + 1});
            return true;
        }
        return false;
    }
    void cancel() override { cancelled_ = true; input_->cancel(); }
    void close() override {
        input_->close();
        inputs_.clear();   // 先关闭归并输入句柄，再删除 run 文件（Windows 文件锁语义）。
        queue_.reset();
        runs_.clear();
        closed_ = true;
    }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.tempFileBytes = runs_.bytes;
        usage.runCount = runs_.runs.size();
        usage.rowsProduced = rows_;
        return usage;
    }
private:
    struct Cursor { Row row; std::size_t run; std::size_t sequence; };
    using CursorQueue = std::priority_queue<Cursor, std::vector<Cursor>,
        std::function<bool(const Cursor&, const Cursor&)>>;
    void prepare() {
        if (prepared_) return;
        prepared_ = true;
        if (cancelled_) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        std::filesystem::create_directories(directory_);
        std::vector<Row> buffer;
        buffer.reserve(runSize_);
        Row row;
        std::size_t runIndex = 0;
        while (input_->next(row)) {
            if (checkCancelled_) checkCancelled_();
            buffer.push_back(std::move(row));
            if (buffer.size() >= runSize_) {
                std::stable_sort(buffer.begin(), buffer.end(), less_);
                writeRun(buffer, directory_, prefix_, runIndex++, runs_);
                buffer.clear(); buffer.reserve(runSize_);
            }
        }
        if (runIndex > 0) {
            if (!buffer.empty()) {
                std::stable_sort(buffer.begin(), buffer.end(), less_);
                writeRun(buffer, directory_, prefix_, runIndex++, runs_);
                buffer.clear();
            }
            std::function<bool(const Cursor&, const Cursor&)> later = [less = less_](
                const Cursor& a, const Cursor& b) {
                if (less(a.row, b.row)) return false;
                if (less(b.row, a.row)) return true;
                return a.run > b.run || (a.run == b.run && a.sequence > b.sequence);
            };
            inputs_.reserve(runs_.runs.size());
            for (std::size_t index = 0; index < runs_.runs.size(); ++index) {
                const auto metadata = detail::readMetadata(runs_.metadata[index]);
                const auto expectedRows = metadata.at("rows").get<std::size_t>();
                const auto expectedChecksum = metadata.at("checksum").get<std::string>();
                std::ifstream verification(runs_.runs[index], std::ios::binary);
                std::uint64_t checksum = detail::checksumBytes("");
                std::size_t actualRows = 0;
                std::string line;
                while (std::getline(verification, line)) {
                    checksum = detail::checksumBytes(line, checksum);
                    checksum = detail::checksumBytes("\n", checksum);
                    ++actualRows;
                }
                if (actualRows != expectedRows || detail::checksumText(checksum) != expectedChecksum)
                    throw MiniSqlError(ErrorCode::Storage, "External sort run checksum mismatch");
                inputs_.emplace_back(runs_.runs[index], std::ios::binary);
            }
            queue_.emplace(std::move(later));
            for (std::size_t index = 0; index < inputs_.size(); ++index) {
                std::string line;
                if (std::getline(inputs_[index], line) && !line.empty())
                    queue_->push({nlohmann::json::parse(line), index, 0});
            }
        } else {
            std::stable_sort(buffer.begin(), buffer.end(), less_);
            merged_ = std::move(buffer);
        }
    }
    std::unique_ptr<RowStream> input_;
    std::function<bool(const Row&, const Row&)> less_;
    std::size_t runSize_;
    std::filesystem::path directory_;
    std::string prefix_;
    std::function<void()> checkCancelled_;
    bool cancelled_ = false, prepared_ = false, closed_ = false;
    std::size_t rows_ = 0;
    SortRuns runs_;
    std::vector<Row> merged_;
    std::size_t mergedIndex_ = 0;
    std::vector<std::ifstream> inputs_;
    std::optional<CursorQueue> queue_;
};

class BudgetStream final : public RowStream {
public:
    BudgetStream(std::unique_ptr<RowStream> input, RowBudget budget)
        : input_(std::move(input)), budget_(budget) {}
    bool next(Row& row) override {
        if (!input_->next(row)) return false;
        ++produced_;
        if (auto limit = exceeded()) abort_(*limit);
        return true;
    }
    void cancel() override { input_->cancel(); }
    void close() override { input_->close(); }
    ResourceUsage resourceUsage() const override {
        auto usage = input_->resourceUsage();
        usage.rowsProduced = produced_;
        return usage;
    }
private:
    enum class Limit { Row, TempFileBytes, Runs, States };
    std::optional<Limit> exceeded() const {
        const auto usage = input_->resourceUsage();
        if (budget_.maxRows > 0 && produced_ > budget_.maxRows) return Limit::Row;
        if (budget_.maxTempFileBytes > 0 && usage.tempFileBytes > budget_.maxTempFileBytes) return Limit::TempFileBytes;
        if (budget_.maxRuns > 0 && usage.runCount > budget_.maxRuns) return Limit::Runs;
        if (budget_.maxStates > 0 && usage.stateCount > budget_.maxStates) return Limit::States;
        return std::nullopt;
    }
    [[noreturn]] void abort_(Limit limit) {
        input_->cancel();
        input_->close();
        const char* message =
            limit == Limit::Row ? "Result row budget exceeded" :
            limit == Limit::TempFileBytes ? "Temporary file budget exceeded" :
            limit == Limit::Runs ? "Sort run budget exceeded" : "Aggregate state budget exceeded";
        throw MiniSqlError(ErrorCode::Execution, message);
    }
    std::unique_ptr<RowStream> input_;
    RowBudget budget_;
    std::size_t produced_ = 0;
};
}  // namespace

std::unique_ptr<RowStream> filterStream(std::unique_ptr<RowStream> input,
                                        std::function<bool(const Row&)> predicate) {
    return std::make_unique<FilterStream>(std::move(input), std::move(predicate));
}
std::unique_ptr<RowStream> projectStream(std::unique_ptr<RowStream> input,
                                         std::function<Row(const Row&)> projection) {
    return std::make_unique<ProjectStream>(std::move(input), std::move(projection));
}
std::unique_ptr<RowStream> distinctStream(std::unique_ptr<RowStream> input) {
    return std::make_unique<DistinctStream>(std::move(input));
}
std::unique_ptr<RowStream> limitStream(std::unique_ptr<RowStream> input, std::size_t offset, std::uint64_t limit) {
    return std::make_unique<LimitStream>(std::move(input), offset, limit);
}
std::unique_ptr<RowStream> sortStream(std::unique_ptr<RowStream> input,
                                      std::function<bool(const Row&, const Row&)> less, std::size_t runSize,
                                      const std::filesystem::path& directory, const std::string& operationId,
                                      std::function<void()> checkCancelled) {
    return std::make_unique<SortStream>(std::move(input), std::move(less), runSize, directory, operationId,
                                        std::move(checkCancelled));
}
std::unique_ptr<RowStream> budgetStream(std::unique_ptr<RowStream> input, RowBudget budget) {
    return std::make_unique<BudgetStream>(std::move(input), budget);
}

}  // namespace minisql::execution