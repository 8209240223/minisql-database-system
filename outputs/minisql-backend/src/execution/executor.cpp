#include "minisql/execution/executor.hpp"
#include "minisql/common/error.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <queue>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace minisql::execution {
namespace {
using json = nlohmann::json;

std::size_t rowBytes(const json& row) {
    const auto bytes = row.dump().size() + 1;
    return bytes > std::numeric_limits<std::size_t>::max() - 64 ? bytes : bytes + 64;
}

std::string artifactId(std::string value) {
    if (value.empty()) return "local";
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '-' && character != '_') character = '-';
    }
    return value;
}

[[noreturn]] void resourceFailure(const std::string& message) {
    throw MiniSqlError(ErrorCode::Execution, message);
}

json parseRunRow(const std::string& line) {
    try { return json::parse(line); }
    catch (const json::exception&) { throw MiniSqlError(ErrorCode::Storage, "Malformed query spill file"); }
}
}

QueryResourceManager::MemoryReservation::MemoryReservation(QueryResourceManager* owner, std::size_t bytes)
    : owner_(owner), bytes_(bytes) {}
QueryResourceManager::MemoryReservation::MemoryReservation(MemoryReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), bytes_(std::exchange(other.bytes_, 0)) {}
QueryResourceManager::MemoryReservation& QueryResourceManager::MemoryReservation::operator=(MemoryReservation&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}
QueryResourceManager::MemoryReservation::~MemoryReservation() { reset(); }
void QueryResourceManager::MemoryReservation::reset() {
    if (owner_) owner_->releaseMemory(bytes_);
    owner_ = nullptr;
    bytes_ = 0;
}

QueryResourceManager::TempArtifact::TempArtifact(QueryResourceManager& owner, std::filesystem::path path)
    : owner_(&owner), path_(std::move(path)) { owner_->artifactCreated(); }
QueryResourceManager::TempArtifact::~TempArtifact() { cleanup(); }
void QueryResourceManager::TempArtifact::account(std::size_t bytes) {
    owner_->acquireDisk(bytes);
    bytes_ += bytes;
}
void QueryResourceManager::TempArtifact::cleanup() {
    if (cleaned_) return;
    std::error_code error;
    const auto removed = std::filesystem::remove(path_, error);
    if (error) return;
    if (!removed) {
        std::error_code existsError;
        if (std::filesystem::exists(path_, existsError) || existsError) return;
    }
    owner_->releaseDisk(bytes_);
    owner_->artifactCleaned();
    cleaned_ = true;
}

QueryResourceManager::QueryResourceManager(std::size_t memoryLimitBytes, std::uint64_t tempDiskLimitBytes)
    : memoryLimitBytes_(memoryLimitBytes), tempDiskLimitBytes_(tempDiskLimitBytes) {
    if (memoryLimitBytes_ == 0) resourceFailure("Query memory budget must be positive");
    if (tempDiskLimitBytes_ == 0) resourceFailure("Temporary disk budget must be positive");
}
QueryResourceManager::MemoryReservation QueryResourceManager::reserveMemory(std::size_t bytes) {
    acquireMemory(bytes);
    return MemoryReservation(this, bytes);
}
std::unique_ptr<QueryResourceManager::TempArtifact> QueryResourceManager::createTempArtifact(const std::filesystem::path& path) {
    return std::make_unique<TempArtifact>(*this, path);
}
void QueryResourceManager::acquireMemory(std::size_t bytes) {
    if (bytes > memoryLimitBytes_ || memoryCurrentBytes_ > memoryLimitBytes_ - bytes)
        resourceFailure("Query memory budget exceeded");
    memoryCurrentBytes_ += bytes;
    memoryPeakBytes_ = std::max(memoryPeakBytes_, memoryCurrentBytes_);
}
void QueryResourceManager::releaseMemory(std::size_t bytes) noexcept {
    memoryCurrentBytes_ = bytes > memoryCurrentBytes_ ? 0 : memoryCurrentBytes_ - bytes;
}
void QueryResourceManager::acquireDisk(std::size_t bytes) {
    if (bytes > tempDiskLimitBytes_ || tempDiskCurrentBytes_ > tempDiskLimitBytes_ - bytes)
        resourceFailure("Temporary disk budget exceeded");
    tempDiskCurrentBytes_ += bytes;
    tempDiskPeakBytes_ = std::max(tempDiskPeakBytes_, tempDiskCurrentBytes_);
    spillBytes_ += bytes;
}
void QueryResourceManager::releaseDisk(std::size_t bytes) noexcept {
    tempDiskCurrentBytes_ = bytes > tempDiskCurrentBytes_ ? 0 : tempDiskCurrentBytes_ - bytes;
}
void QueryResourceManager::artifactCreated() { ++spillFiles_; }
void QueryResourceManager::artifactCleaned() noexcept { ++tempFilesCleaned_; }
json QueryResourceManager::usage() const {
    return {{"memoryLimitBytes", memoryLimitBytes_}, {"memoryCurrentBytes", memoryCurrentBytes_},
        {"memoryPeakBytes", memoryPeakBytes_}, {"tempDiskLimitBytes", tempDiskLimitBytes_},
        {"tempDiskCurrentBytes", tempDiskCurrentBytes_}, {"tempDiskPeakBytes", tempDiskPeakBytes_},
        {"spillBytes", spillBytes_}, {"spillFiles", spillFiles_}, {"tempFilesCleaned", tempFilesCleaned_}};
}

MappingRowStream::MappingRowStream(std::unique_ptr<RowStream> child, RowTransform transform,
                                   std::optional<json> emptyInputRow)
    : child_(std::move(child)), transform_(std::move(transform)), emptyInputRow_(std::move(emptyInputRow)) {}
bool MappingRowStream::next(json& row) {
    json input;
    if (child_->next(input)) {
        sawInput_ = true;
        row = transform_(input);
        ++rows_;
        return true;
    }
    if (!sawInput_ && !emittedEmpty_ && emptyInputRow_) {
        emittedEmpty_ = true;
        row = *emptyInputRow_;
        ++rows_;
        return true;
    }
    return false;
}
void MappingRowStream::cancel() { child_->cancel(); }
void MappingRowStream::close() { child_->close(); }
json MappingRowStream::resourceUsage() const {
    return {{"kind", "MappingRowStream"}, {"rows", rows_}, {"child", child_->resourceUsage()}};
}

struct ExternalSortRowStream::Implementation {
    struct Run { std::unique_ptr<QueryResourceManager::TempArtifact> artifact; std::size_t rows = 0; };
    struct Cursor { json row; std::size_t run = 0; std::size_t sequence = 0; };
    struct Later {
        RowCompare compare;
        bool operator()(const Cursor& left, const Cursor& right) const {
            if (compare(left.row, right.row)) return false;
            if (compare(right.row, left.row)) return true;
            return left.run > right.run || (left.run == right.run && left.sequence > right.sequence);
        }
    };
    using Queue = std::priority_queue<Cursor, std::vector<Cursor>, Later>;

    std::unique_ptr<RowStream> child;
    RowCompare compare;
    std::shared_ptr<QueryResourceManager> resources;
    std::filesystem::path directory;
    std::string operationId;
    std::size_t maxRows;
    CancelCheck checkCancelled;
    std::optional<std::size_t> outputWidth;
    std::optional<std::size_t> topN;
    // Top-N 上限：有值时只保留排序结果的前 N 行。
    std::size_t discarded = 0;
    // 被丢弃的行数：供资源用量与调试观察。
    std::vector<json> memoryRows;
    std::vector<QueryResourceManager::MemoryReservation> memoryReservations;
    std::vector<Run> runs;
    std::vector<std::ifstream> inputs;
    std::vector<QueryResourceManager::MemoryReservation> cursorReservations;
    std::unique_ptr<Queue> queue;
    std::size_t emitted = 0;
    std::size_t inputRows = 0;
    std::size_t nextArtifact = 0;
    bool prepared = false;
    bool external = false;
    bool cancelled = false;
    bool closed = false;

    Implementation(std::unique_ptr<RowStream> input, RowCompare comparator,
                   std::shared_ptr<QueryResourceManager> manager,
                   std::filesystem::path tempDirectory, std::string id,
                   std::size_t rowLimit, CancelCheck cancel,
                   std::optional<std::size_t> width, std::optional<std::size_t> limit)
        : child(std::move(input)), compare(std::move(comparator)), resources(std::move(manager)),
          directory(std::move(tempDirectory)), operationId(artifactId(std::move(id))),
          maxRows(std::max<std::size_t>(1, rowLimit)), checkCancelled(std::move(cancel)), outputWidth(width), topN(limit) {}

    void check() const {
        if (cancelled) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        if (checkCancelled) checkCancelled();
    }
    std::filesystem::path newPath() {
        return directory / ("query-" + operationId + "-" + std::to_string(nextArtifact++) + ".jsonl");
    }
    void writeRow(std::ofstream& output, QueryResourceManager::TempArtifact& artifact, const json& row) {
        const auto line = row.dump();
        artifact.account(line.size() + 1);
        output << line << '\n';
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write query spill file");
    }
    Run spillRows(std::vector<json>& rows) {
        check();
        std::stable_sort(rows.begin(), rows.end(), compare);
        std::filesystem::create_directories(directory);
        auto artifact = resources->createTempArtifact(newPath());
        std::ofstream output(artifact->path(), std::ios::binary | std::ios::trunc);
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create query spill file");
        for (const auto& row : rows) writeRow(output, *artifact, row);
        output.close();
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot close query spill file");
        Run run{std::move(artifact), rows.size()};
        rows.clear();
        memoryReservations.clear();
        return run;
    }
    bool readCursor(std::ifstream& input, std::size_t run, std::size_t sequence, Cursor& cursor) {
        std::string line;
        if (!std::getline(input, line)) return false;
        cursor = {parseRunRow(line), run, sequence};
        if (rowBytes(cursor.row) > resources->memoryLimitBytes()) resourceFailure("Spill row exceeds query memory budget");
        return true;
    }
    Run mergeGroup(std::vector<Run>& source, std::size_t begin, std::size_t end) {
        auto artifact = resources->createTempArtifact(newPath());
        std::ofstream output(artifact->path(), std::ios::binary | std::ios::trunc);
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create merged spill file");
        std::vector<std::ifstream> readers;
        readers.reserve(end - begin);
        std::vector<QueryResourceManager::MemoryReservation> reservations(end - begin);
        Queue pending(Later{compare});
        for (std::size_t index = begin; index < end; ++index) {
            readers.emplace_back(source[index].artifact->path(), std::ios::binary);
            if (!readers.back()) throw MiniSqlError(ErrorCode::Storage, "Cannot read query spill file");
            Cursor cursor;
            if (readCursor(readers.back(), index - begin, 0, cursor)) {
                reservations[index - begin] = resources->reserveMemory(rowBytes(cursor.row));
                pending.push(std::move(cursor));
            }
        }
        std::size_t rows = 0;
        while (!pending.empty()) {
            check();
            auto cursor = pending.top();
            pending.pop();
            writeRow(output, *artifact, cursor.row);
            ++rows;
            reservations[cursor.run].reset();
            Cursor next;
            if (readCursor(readers[cursor.run], cursor.run, cursor.sequence + 1, next)) {
                reservations[cursor.run] = resources->reserveMemory(rowBytes(next.row));
                pending.push(std::move(next));
            }
        }
        output.close();
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot close merged spill file");
        for (auto& reader : readers) reader.close();
        for (std::size_t index = begin; index < end; ++index) source[index].artifact->cleanup();
        return {std::move(artifact), rows};
    }
    void reduceRuns() {
        const auto fanIn = std::max<std::size_t>(2, std::min<std::size_t>(32, resources->memoryLimitBytes() / 512));
        while (runs.size() > fanIn) {
            std::vector<Run> merged;
            for (std::size_t begin = 0; begin < runs.size(); begin += fanIn)
                merged.push_back(mergeGroup(runs, begin, std::min(runs.size(), begin + fanIn)));
            runs = std::move(merged);
        }
    }
    void openRuns() {
        inputs.reserve(runs.size());
        cursorReservations.resize(runs.size());
        queue = std::make_unique<Queue>(Later{compare});
        for (std::size_t index = 0; index < runs.size(); ++index) {
            inputs.emplace_back(runs[index].artifact->path(), std::ios::binary);
            if (!inputs.back()) throw MiniSqlError(ErrorCode::Storage, "Cannot read query spill file");
            Cursor cursor;
            if (readCursor(inputs.back(), index, 0, cursor)) {
                cursorReservations[index] = resources->reserveMemory(rowBytes(cursor.row));
                queue->push(std::move(cursor));
            }
        }
    }
    void prepare() {
        if (prepared) return;
        prepared = true;
        // 有界排序：上方只要前 N 行时，没必要把全表排序。
        if (topN) { prepareTopN(*topN); return; }
        // 完整排序：需要全部有序结果（如没有 LIMIT 的 ORDER BY）。
        json row;
        while (child->next(row)) {
            check();
            const auto bytes = rowBytes(row);
            if (bytes > resources->memoryLimitBytes()) resourceFailure("Single row exceeds query memory budget");
            const auto current = resources->usage().at("memoryCurrentBytes").get<std::size_t>();
            if (!memoryRows.empty() && (memoryRows.size() >= maxRows || current + bytes > resources->memoryLimitBytes())) {
                runs.push_back(spillRows(memoryRows));
                external = true;
            }
            memoryReservations.push_back(resources->reserveMemory(bytes));
            memoryRows.push_back(std::move(row));
            ++inputRows;
        }
        child->close();
        if (!external) {
            std::stable_sort(memoryRows.begin(), memoryRows.end(), compare);
            return;
        }
        if (!memoryRows.empty()) runs.push_back(spillRows(memoryRows));
        reduceRuns();
        openRuns();
    }
    void prepareTopN(std::size_t limit) {
    // 有界排序：只保留当前最优的 limit 行。
    // 读满后，新行只在优于「缓冲区里最差的一行」时才替换它，否则直接丢弃。
    // 内存占用与排序代价因此只与 limit 相关，与输入行数无关。
    //
    // precedes(a, b) 为真表示 a 必须排在 b 之前，语义与完整排序的 std::stable_sort 一致：
    // 主键能分先后时按主键；主键并列时按输入序号，靠前的在前。
    // 并列决胜必须显式写出：Top-N 会把行丢弃，若并列时任意取舍，
    // 保留下来的行与完整排序的前 N 行就会不同（这是实测发现过的缺陷）。
        const std::size_t bound = std::max<std::size_t>(1, limit);
        // 至少保留一行，避免 limit=0 时边界处理变得特殊。
        struct Entry { json row; std::size_t sequence; };
        // 缓冲区元素：行 + 它的输入序号。
        std::vector<Entry> buffer;
        // 有界缓冲区，最多 bound 个元素。
        buffer.reserve(bound);
        // 预留容量，避免反复扩容。
        std::size_t sequence = 0;
        // 输入序号，从 0 开始递增。
        const auto precedes = [&](const Entry& left, const Entry& right) {
        // 判定 left 是否必须排在 right 之前。
            if (compare(left.row, right.row)) return true;
            // 主键决定 left 在前。
            if (compare(right.row, left.row)) return false;
            // 主键决定 right 在前。
            return left.sequence < right.sequence;
            // 主键并列：输入序号小的在前，复现稳定排序语义。
        };
        // precedes 定义结束。
        json row;
        // 复用同一个 JSON 对象接收每一行。
        while (child->next(row)) {
        // 一直读到输入结束。
            check();
            // 响应取消与超时。
            const auto bytes = rowBytes(row);
            // 本行的内存体积。
            if (bytes > resources->memoryLimitBytes()) resourceFailure("Single row exceeds query memory budget");
            // 单行超预算直接报错，与完整排序保持一致。
            ++inputRows;
            // 输入行数照样累计，方便对比。
            Entry entry{std::move(row), sequence++};
            // 记下这一行与它的输入序号。
            if (buffer.size() < bound) {
            // 还没填满：直接收下。
                memoryReservations.push_back(resources->reserveMemory(bytes));
                buffer.push_back(std::move(entry));
                continue;
            }
            // 已经满了：找出缓冲区里排在最末尾的那一个（最差者）。
            std::size_t worst = 0;
            for (std::size_t k = 1; k < buffer.size(); ++k)
                if (precedes(buffer[worst], buffer[k])) worst = k;
                // 若最差者排在候选 k 之前，说明 k 更靠后，更新最差者。
            if (precedes(entry, buffer[worst])) {
            // 新行排在最差者之前，说明新行更好：替换掉它。
                memoryReservations[worst] = resources->reserveMemory(bytes);
                // 重新计账内存（旧预留会在赋值时释放）。
                buffer[worst] = std::move(entry);
                // 用新行替换掉最差的那一行。
            } else {
            // 新行排不进前 bound 名：直接丢弃。
                ++discarded;
                // 记一个丢弃计数。
            }
            // 替换或丢弃分支结束。
        }
        // 输入读完。
        child->close();
        // 关闭子流释放资源。
        std::stable_sort(buffer.begin(), buffer.end(), precedes);
        // 缓冲区排序：用同一个 precedes，结果与完整 stable_sort 的前 N 行完全一致。
        memoryRows.reserve(buffer.size());
        // 结果行数不会再超过 bound。
        for (auto& item : buffer) memoryRows.push_back(std::move(item.row));
        // 把行搬进最终输出缓冲区。
    }
    bool next(json& row) {
        prepare();
        check();
        if (!external) {
            if (emitted >= memoryRows.size()) return false;
            row = std::move(memoryRows[emitted++]);
        } else {
            if (!queue || queue->empty()) return false;
            auto cursor = queue->top();
            queue->pop();
            row = std::move(cursor.row);
            ++emitted;
            cursorReservations[cursor.run].reset();
            Cursor next;
            if (readCursor(inputs[cursor.run], cursor.run, cursor.sequence + 1, next)) {
                cursorReservations[cursor.run] = resources->reserveMemory(rowBytes(next.row));
                queue->push(std::move(next));
            }
        }
        if (outputWidth && row.is_array()) while (row.size() > *outputWidth) row.erase(row.end() - 1);
        return true;
    }
    void close() {
        if (closed) return;
        child->close();
        for (auto& input : inputs) input.close();
        queue.reset();
        cursorReservations.clear();
        memoryRows.clear();
        memoryReservations.clear();
        for (auto& run : runs) run.artifact->cleanup();
        runs.clear();
        closed = true;
    }
    json usage() const {
        auto usage = resources->usage();
        usage["kind"] = "Sort";
        usage["rows"] = emitted;
        usage["inputRows"] = inputRows;
        usage["external"] = external;
        if (topN) { usage["topN"] = *topN; usage["discardedRows"] = discarded; }
        // 有界排序时额外报告上限与丢弃行数，便于确认优化真的生效了。
        usage["maxRows"] = maxRows;
        usage["child"] = child->resourceUsage();
        return usage;
    }
};

ExternalSortRowStream::ExternalSortRowStream(std::unique_ptr<RowStream> child, RowCompare compare,
                                             std::shared_ptr<QueryResourceManager> resources,
                                             std::filesystem::path directory, std::string operationId,
                                             std::size_t maxRows, CancelCheck checkCancelled,
                                             std::optional<std::size_t> outputWidth,
                                             std::optional<std::size_t> topN)
    : implementation_(std::make_unique<Implementation>(std::move(child), std::move(compare), std::move(resources),
          std::move(directory), std::move(operationId), maxRows, std::move(checkCancelled), outputWidth, topN)) {}
ExternalSortRowStream::~ExternalSortRowStream() { implementation_->close(); }
bool ExternalSortRowStream::next(json& row) { return implementation_->next(row); }
void ExternalSortRowStream::cancel() {
    implementation_->cancelled = true;
    implementation_->child->cancel();
    implementation_->close();
}
void ExternalSortRowStream::close() { implementation_->close(); }
json ExternalSortRowStream::resourceUsage() const { return implementation_->usage(); }

DistinctRowStream::DistinctRowStream(std::unique_ptr<RowStream> sorted,
                                     std::shared_ptr<QueryResourceManager> resources)
    : sorted_(std::move(sorted)), resources_(std::move(resources)) {}
bool DistinctRowStream::next(json& row) {
    json candidate;
    while (sorted_->next(candidate)) {
        if (previous_ && *previous_ == candidate) { ++duplicates_; continue; }
        previous_ = candidate;
        row = std::move(candidate);
        ++rows_;
        return true;
    }
    return false;
}
void DistinctRowStream::cancel() { sorted_->cancel(); }
void DistinctRowStream::close() { sorted_->close(); previous_.reset(); }
json DistinctRowStream::resourceUsage() const {
    auto usage = resources_->usage();
    usage["kind"] = "Distinct";
    usage["rows"] = rows_;
    usage["duplicates"] = duplicates_;
    usage["child"] = sorted_->resourceUsage();
    return usage;
}

GroupedAggregateRowStream::GroupedAggregateRowStream(std::unique_ptr<RowStream> sortedRecords,
                                                     json initialState, AggregateCombine combine,
                                                     AggregateFinalize finalize,
                                                     std::shared_ptr<QueryResourceManager> resources)
    : sortedRecords_(std::move(sortedRecords)), initialState_(std::move(initialState)),
      combine_(std::move(combine)), finalize_(std::move(finalize)), resources_(std::move(resources)) {}
bool GroupedAggregateRowStream::next(json& row) {
    json record;
    if (pending_) { record = std::move(*pending_); pending_.reset(); }
    else if (!sortedRecords_->next(record)) return false;
    if (!record.is_object() || !record.contains("key") || !record.contains("values"))
        throw MiniSqlError(ErrorCode::Execution, "Aggregate spill record schema mismatch");
    const auto currentKey = record.at("key");
    auto state = initialState_;
    combine_(state, record.at("values"));
    while (sortedRecords_->next(record)) {
        if (record.at("key") != currentKey) { pending_ = std::move(record); break; }
        combine_(state, record.at("values"));
    }
    row = finalize_(currentKey, state);
    ++groups_;
    return true;
}
void GroupedAggregateRowStream::cancel() { sortedRecords_->cancel(); }
void GroupedAggregateRowStream::close() { sortedRecords_->close(); pending_.reset(); }
json GroupedAggregateRowStream::resourceUsage() const {
    auto usage = resources_->usage();
    usage["kind"] = "Aggregate";
    usage["rows"] = groups_;
    usage["groups"] = groups_;
    usage["child"] = sortedRecords_->resourceUsage();
    return usage;
}

struct JoinRowStream::Implementation {
    std::unique_ptr<RowStream> left;
    std::unique_ptr<RowStream> right;
    std::function<bool(const json&)> predicate;
    std::shared_ptr<QueryResourceManager> resources;
    std::filesystem::path directory;
    std::string operationId;
    std::size_t maxRows;
    std::size_t rightWidth;
    bool leftOuter;
    std::optional<std::pair<std::size_t, std::size_t>> hashKeys;
    CancelCheck checkCancelled;
    std::vector<json> rightRows;
    std::vector<QueryResourceManager::MemoryReservation> rightReservations;
    std::vector<QueryResourceManager::MemoryReservation> hashReservations;
    std::unordered_map<std::string, std::vector<std::size_t>> buckets;
    std::unique_ptr<QueryResourceManager::TempArtifact> spool;
    std::ifstream spoolInput;
    std::vector<std::unique_ptr<QueryResourceManager::TempArtifact>> hashPartitions;
    std::size_t hashPartitionCount = 0;
    std::size_t activeHashPartition = std::numeric_limits<std::size_t>::max();
    json leftRow;
    const std::vector<std::size_t>* matches = nullptr;
    std::size_t matchCursor = 0;
    std::size_t rightCursor = 0;
    std::size_t rows = 0;
    std::size_t leftRows = 0;
    std::size_t rightRowCount = 0;
    bool leftActive = false;
    bool leftMatched = false;
    bool prepared = false;
    bool spilled = false;
    bool cancelled = false;
    bool closed = false;

    Implementation(std::unique_ptr<RowStream> lhs, std::unique_ptr<RowStream> rhs,
                   std::function<bool(const json&)> condition,
                   std::shared_ptr<QueryResourceManager> manager,
                   std::filesystem::path tempDirectory, std::string id,
                   std::size_t rowLimit, std::size_t width, bool outer,
                   std::optional<std::pair<std::size_t, std::size_t>> keys,
                   CancelCheck cancel)
        : left(std::move(lhs)), right(std::move(rhs)), predicate(std::move(condition)),
          resources(std::move(manager)), directory(std::move(tempDirectory)), operationId(artifactId(std::move(id))),
          maxRows(std::max<std::size_t>(1, rowLimit)), rightWidth(width), leftOuter(outer),
          hashKeys(keys), checkCancelled(std::move(cancel)) {}
    void check() const {
        if (cancelled) throw MiniSqlError(ErrorCode::Cancelled, "Query cancelled");
        if (checkCancelled) checkCancelled();
    }
    void writeArtifactRow(QueryResourceManager::TempArtifact& artifact, std::ofstream& output, const json& row) {
        const auto line = row.dump();
        artifact.account(line.size() + 1);
        output << line << '\n';
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write join spill file");
    }
    void writeSpoolRow(std::ofstream& output, const json& row) { writeArtifactRow(*spool, output, row); }
    std::size_t partitionFor(const json& value) const {
        return std::hash<std::string>{}(value.dump()) % hashPartitions.size();
    }
    void partitionSpilledHash() {
        const auto memoryTarget = std::max<std::size_t>(1, resources->memoryLimitBytes() / 2);
        const auto spilledBytes = resources->usage().at("tempDiskCurrentBytes").get<std::uint64_t>();
        const auto estimated = static_cast<std::size_t>(std::max<std::uint64_t>(2, (spilledBytes + memoryTarget - 1) / memoryTarget));
        const auto count = std::clamp<std::size_t>(estimated * 2, 2, 256);
        hashPartitionCount = count;
        hashPartitions.reserve(count);
        std::vector<std::ofstream> outputs(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto artifact = resources->createTempArtifact(directory / ("query-" + operationId + "-hash-" + std::to_string(index) + ".jsonl"));
            outputs[index].open(artifact->path(), std::ios::binary | std::ios::trunc);
            if (!outputs[index]) throw MiniSqlError(ErrorCode::Storage, "Cannot create hash join partition");
            hashPartitions.push_back(std::move(artifact));
        }
        std::ifstream input(spool->path(), std::ios::binary);
        if (!input) throw MiniSqlError(ErrorCode::Storage, "Cannot read join spill file");
        std::string line;
        while (std::getline(input, line)) {
            check();
            const auto row = parseRunRow(line);
            const auto keyIndex = hashKeys->second;
            if (keyIndex >= row.size()) throw MiniSqlError(ErrorCode::Execution, "HashJoin right key outside row");
            const auto& value = row.at(keyIndex);
            if (value.is_null()) continue;
            const auto partition = partitionFor(value);
            writeArtifactRow(*hashPartitions[partition], outputs[partition], row);
        }
        for (auto& output : outputs) {
            output.close();
            if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot close hash join partition");
        }
        input.close();
        spool->cleanup();
        spool.reset();
    }
    void loadHashPartition(std::size_t partition) {
        if (activeHashPartition == partition) return;
        buckets.clear();
        hashReservations.clear();
        rightRows.clear();
        rightReservations.clear();
        std::ifstream input(hashPartitions.at(partition)->path(), std::ios::binary);
        if (!input) throw MiniSqlError(ErrorCode::Storage, "Cannot read hash join partition");
        std::string line;
        while (std::getline(input, line)) {
            check();
            auto row = parseRunRow(line);
            const auto bytes = rowBytes(row);
            rightReservations.push_back(resources->reserveMemory(bytes));
            rightRows.push_back(std::move(row));
            const auto index = rightRows.size() - 1;
            const auto& value = rightRows.back().at(hashKeys->second);
            const auto serialized = value.dump();
            const auto found = buckets.find(serialized);
            const auto bucketBytes = sizeof(std::size_t) + (found == buckets.end() ? serialized.size() + 64 : 0);
            hashReservations.push_back(resources->reserveMemory(bucketBytes));
            buckets[serialized].push_back(index);
        }
        activeHashPartition = partition;
    }
    void prepare() {
        if (prepared) return;
        prepared = true;
        const auto memoryTarget = std::max<std::size_t>(1, resources->memoryLimitBytes() / 2);
        std::ofstream output;
        json row;
        while (right->next(row)) {
            check();
            const auto bytes = rowBytes(row);
            if (bytes > resources->memoryLimitBytes()) resourceFailure("Single join row exceeds query memory budget");
            const auto current = resources->usage().at("memoryCurrentBytes").get<std::size_t>();
            if (!spilled && (rightRows.size() >= maxRows || current + bytes > memoryTarget)) {
                std::filesystem::create_directories(directory);
                spool = resources->createTempArtifact(directory / ("query-" + operationId + "-join.jsonl"));
                output.open(spool->path(), std::ios::binary | std::ios::trunc);
                if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create join spill file");
                for (const auto& buffered : rightRows) writeSpoolRow(output, buffered);
                rightRows.clear();
                rightReservations.clear();
                spilled = true;
            }
            if (spilled) writeSpoolRow(output, row);
            else {
                rightReservations.push_back(resources->reserveMemory(bytes));
                rightRows.push_back(std::move(row));
            }
            ++rightRowCount;
        }
        right->close();
        if (output.is_open()) {
            output.close();
            if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot close join spill file");
        }
        if (!spilled && hashKeys) {
            for (std::size_t index = 0; index < rightRows.size(); ++index) {
                const auto keyIndex = hashKeys->second;
                if (keyIndex >= rightRows[index].size()) throw MiniSqlError(ErrorCode::Execution, "HashJoin right key outside row");
                const auto& value = rightRows[index].at(keyIndex);
                if (!value.is_null()) {
                    const auto serialized = value.dump();
                    auto found = buckets.find(serialized);
                    const auto bytes = sizeof(std::size_t) + (found == buckets.end() ? serialized.size() + 64 : 0);
                    const auto current = resources->usage().at("memoryCurrentBytes").get<std::size_t>();
                    if (bytes > resources->memoryLimitBytes() - current) {
                        buckets.clear();
                        hashReservations.clear();
                        hashKeys.reset();
                        break;
                    }
                    hashReservations.push_back(resources->reserveMemory(bytes));
                    buckets[serialized].push_back(index);
                }
            }
        } else if (spilled && hashKeys) {
            partitionSpilledHash();
        }
    }
    bool startLeft() {
        if (!left->next(leftRow)) return false;
        ++leftRows;
        leftActive = true;
        leftMatched = false;
        rightCursor = 0;
        matchCursor = 0;
        matches = nullptr;
        if (spilled && !hashKeys) {
            spoolInput.close();
            spoolInput.open(spool->path(), std::ios::binary);
            if (!spoolInput) throw MiniSqlError(ErrorCode::Storage, "Cannot read join spill file");
        } else if (hashKeys) {
            const auto keyIndex = hashKeys->first;
            if (keyIndex >= leftRow.size()) throw MiniSqlError(ErrorCode::Execution, "HashJoin left key outside row");
            const auto& value = leftRow.at(keyIndex);
            if (!value.is_null()) {
                if (spilled) loadHashPartition(partitionFor(value));
                const auto found = buckets.find(value.dump());
                if (found != buckets.end()) matches = &found->second;
            }
        }
        return true;
    }
    bool candidate(const json& rightRow, json& output) {
        json combined = leftRow;
        for (const auto& value : rightRow) combined.push_back(value);
        if (!predicate(combined)) return false;
        leftMatched = true;
        output = std::move(combined);
        ++rows;
        return true;
    }
    bool next(json& row) {
        prepare();
        for (;;) {
            check();
            if (!leftActive && !startLeft()) return false;
            if (spilled && !hashKeys) {
                std::string line;
                while (std::getline(spoolInput, line)) {
                    check();
                    auto rightRow = parseRunRow(line);
                    if (candidate(rightRow, row)) return true;
                }
            } else if (hashKeys) {
                while (matches && matchCursor < matches->size()) {
                    check();
                    const auto& rightRow = rightRows[matches->at(matchCursor++)];
                    if (candidate(rightRow, row)) return true;
                }
            } else {
                while (rightCursor < rightRows.size()) {
                    check();
                    const auto& rightRow = rightRows[rightCursor++];
                    if (candidate(rightRow, row)) return true;
                }
            }
            if (leftOuter && !leftMatched) {
                row = leftRow;
                for (std::size_t index = 0; index < rightWidth; ++index) row.push_back(nullptr);
                leftActive = false;
                ++rows;
                return true;
            }
            leftActive = false;
        }
    }
    void close() {
        if (closed) return;
        left->close();
        right->close();
        spoolInput.close();
        buckets.clear();
        hashReservations.clear();
        rightRows.clear();
        rightReservations.clear();
        if (spool) spool->cleanup();
        for (auto& partition : hashPartitions) if (partition) partition->cleanup();
        hashPartitions.clear();
        closed = true;
    }
    json usage() const {
        auto usage = resources->usage();
        usage["kind"] = hashKeys ? "HashJoin" : leftOuter ? "LeftJoin" : "NestedLoopJoin";
        usage["rows"] = rows;
        usage["leftRows"] = leftRows;
        usage["rightRows"] = rightRowCount;
        usage["external"] = spilled;
        usage["strategy"] = spilled && hashKeys ? "partitioned-hash" : spilled ? "spilled-nested-loop" : hashKeys ? "in-memory-hash" : "in-memory-nested-loop";
        if (spilled && hashKeys) usage["partitions"] = hashPartitionCount;
        usage["left"] = left->resourceUsage();
        usage["right"] = right->resourceUsage();
        return usage;
    }
};

JoinRowStream::JoinRowStream(std::unique_ptr<RowStream> left, std::unique_ptr<RowStream> right,
                             std::function<bool(const json&)> predicate,
                             std::shared_ptr<QueryResourceManager> resources,
                             std::filesystem::path directory, std::string operationId,
                             std::size_t maxRows, std::size_t rightWidth, bool leftOuter,
                             std::optional<std::pair<std::size_t, std::size_t>> hashKeys,
                             CancelCheck checkCancelled)
    : implementation_(std::make_unique<Implementation>(std::move(left), std::move(right), std::move(predicate),
          std::move(resources), std::move(directory), std::move(operationId), maxRows, rightWidth, leftOuter,
          hashKeys, std::move(checkCancelled))) {}
JoinRowStream::~JoinRowStream() { implementation_->close(); }
bool JoinRowStream::next(json& row) { return implementation_->next(row); }
void JoinRowStream::cancel() {
    implementation_->cancelled = true;
    implementation_->left->cancel();
    implementation_->right->cancel();
    implementation_->close();
}
void JoinRowStream::close() { implementation_->close(); }
json JoinRowStream::resourceUsage() const { return implementation_->usage(); }

} // namespace minisql::execution
