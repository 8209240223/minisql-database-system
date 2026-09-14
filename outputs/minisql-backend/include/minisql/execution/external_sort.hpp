#pragma once
#include "minisql/common/error.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace minisql::execution {
namespace detail {
inline std::string sortArtifactId(std::string value) {
// 把会话编号这类文本清洗成可以安全放进文件名的形式。
    if (value.empty()) return "local";
    // 空串统一映射成 local，避免生成形如 sort--0.jsonl 的怪名字。
    for (char& character : value) {
    // 逐个字符检查。
        const auto byte = static_cast<unsigned char>(character);
        // 取无符号字节值，避免负值传入 ctype 造成未定义行为。
        if (!std::isalnum(byte) && character != '-' && character != '_') character = '-';
        // 只保留字母、数字、减号、下划线，其余一律替换成减号。
    }
    // 字符替换结束。
    return value;
}
// 函数结束。
inline std::uint64_t checksumBytes(std::string_view bytes, std::uint64_t hash = 1469598103934665603ULL) {
// FNV-1a 64 位校验和；默认参数是 FNV 的标准偏移基数。
    for (const unsigned char byte : bytes) {
    // 逐字节处理。
        hash ^= byte;
        // 先异或再乘素数，这是 FNV-1a 的标准两步。
        hash *= 1099511628211ULL;
        // 乘 FNV 的 64 位素数。
    }
    // 字节处理结束。
    return hash;
}
// 函数结束。
inline std::string checksumText(std::uint64_t checksum) {
// 把 64 位校验和格式化成 16 位定宽的小写十六进制文本。
    std::ostringstream output;
    // 字符串流缓冲。
    output << std::hex << std::setfill('0') << std::setw(16) << checksum;
    // 十六进制输出、补零、宽度 16。
    return output.str();
}
// 函数结束。
inline nlohmann::json readMetadata(const std::filesystem::path& path) {
// 读取一个归并段的元数据文件。
    std::ifstream input(path, std::ios::binary);
    // 以二进制方式打开，避免平台差异影响内容。
    if (!input) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort metadata");
    // 打不开就按存储错误抛出。
    try {
    // 解析 JSON，失败要转成统一的存储错误。
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
    // 捕获 JSON 解析异常。
        throw MiniSqlError(ErrorCode::Storage, "Malformed external sort metadata");
        // 换成"元数据损坏"的存储错误抛出。
    }
}
}

template <typename Rows, typename Compare>
// 外部排序模板：Rows 是行容器类型，Compare 是比较函数类型。
void externalSort(Rows& rows, Compare compare, std::size_t maxRows,
// rows 既是输入也是输出，函数结束时被就地替换成排序后的结果。
                  const std::filesystem::path& directory,
                  // maxRows 是内存中最多容纳的行数，超过就落盘做多路归并。
                  const std::string& operationId = "local",
                  // directory 是放临时归并段的目录。
                  const std::function<void()>& checkCancelled = {}) {
                  // operationId 用于生成可区分、可清理的临时文件名。
    using Row = typename Rows::value_type;
    // checkCancelled 是可选的取消检查回调，长跑排序里要定期调用。
    if (maxRows == 0) throw MiniSqlError(ErrorCode::InvalidArgument, "External sort row budget must be positive");
    // 取出元素类型，后面用它构造归并结果。
    if (rows.size() <= maxRows) {
    // 内存预算必须为正；为 0 无法切段，属于非法参数。
        std::stable_sort(rows.begin(), rows.end(), compare);
        // 快路径：数据本身就能放进内存预算。
        return;
        // 用 stable_sort 排序，相等的元素保持原有相对顺序，这对分页结果很重要。
    }
    // 直接返回，完全不需要碰磁盘。
    std::filesystem::create_directories(directory);
    // 慢路径：先确保临时目录存在。
    std::vector<std::filesystem::path> runs;
    // 记录所有生成的归并段文件，便于最后统一清理。
    std::vector<std::filesystem::path> metadataFiles;
    // 记录所有元数据文件。
    struct ArtifactCleanup {
    // 定义一个 RAII 清理器：无论正常返回还是抛异常，都会删掉临时文件。
        std::vector<std::filesystem::path>& runs;
        // 持有段文件列表的引用。
        std::vector<std::filesystem::path>& metadata;
        // 持有元数据文件列表的引用。
        ~ArtifactCleanup() {
        // 析构时执行清理。
            for (const auto& file : runs) {
            // 逐段文件删除。
                std::error_code error;
                // 用 error_code 版本删除，避免清理阶段因为异常而中断。
                std::filesystem::remove(file, error);
            }
            // 段文件遍历结束。
            for (const auto& file : metadata) {
            // 再删元数据文件。
                std::error_code error;
                // 同样用 error_code 版本。
                std::filesystem::remove(file, error);
            }
            // 元数据文件遍历结束。
        }
    } cleanup{runs, metadataFiles};
    // 构造清理器并绑定两个文件列表。
    const auto prefix = "sort-" + detail::sortArtifactId(operationId) + "-";
    // 统一的文件名前缀，带清洗过的操作标识。
    for (std::size_t begin = 0; begin < rows.size(); begin += maxRows) {
    // 按 maxRows 切成一段一段。
        if (checkCancelled) checkCancelled();
        // 每段开始前先看有没有取消请求。
        const auto end = std::min(rows.size(), begin + maxRows);
        // 计算本段的结束下标，最后一段可能不足 maxRows。
        std::stable_sort(rows.begin() + static_cast<std::ptrdiff_t>(begin), rows.begin() + static_cast<std::ptrdiff_t>(end), compare);
        // 段内用 stable_sort 排序。
        const auto run = directory / (prefix + std::to_string(runs.size()) + ".jsonl");
        // 生成本段文件名，编号就是当前已有段数。
        std::ofstream output(run, std::ios::binary | std::ios::trunc);
        // 打开输出文件并截断写。
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort run");
        // 打开失败按存储错误抛出。
        std::uint64_t checksum = detail::checksumBytes("");
        // 从"空内容"的初始校验和开始累加。
        std::size_t runRows = 0;
        // 记录本段实际写入的行数。
        for (std::size_t index = begin; index < end; ++index) {
        // 逐行写出本段。
            const auto line = rows[index].dump();
            // 把这一行序列化成 JSON 文本。
            output << line << '\n';
            // 写入该行并换行。
            checksum = detail::checksumBytes(line, checksum);
            // 把行内容累加进校验和。
            checksum = detail::checksumBytes("\n", checksum);
            // 再把换行符也累加进去，保证校验和对行边界敏感。
            ++runRows;
            // 行数加一。
        }
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort run");
        // 检查写过程中是否发生错误。
        const auto metadataFile = std::filesystem::path(run.string() + ".meta.json");
        // 生成对应的元数据文件名。
        std::ofstream metadataOutput(metadataFile, std::ios::binary | std::ios::trunc);
        // 打开元数据文件。
        if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort metadata");
        // 打开失败按存储错误抛出。
        metadataOutput << nlohmann::json{{"operationId", operationId}, {"runIndex", runs.size()}, {"rows", runRows},
        // 写出元数据：操作标识、段序号、行数、校验算法与校验和。
            {"checksumAlgorithm", "fnv1a64"}, {"checksum", detail::checksumText(checksum)}}.dump() << '\n';
        if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort metadata");
        // 检查写过程中是否发生错误。
        runs.push_back(run);
        // 把段文件登记进清理列表。
        metadataFiles.push_back(metadataFile);
        // 把元数据文件登记进清理列表。
    }
    struct Cursor { nlohmann::json row; std::size_t run; std::size_t sequence; };
    // 归并游标：当前行、来自哪个段、在该段内的序号（用于保持稳定）。
    struct Later {
    // 归并比较器：优先队列是大顶堆，所以要反过来定义"谁应该排在后面"。
        Compare compare;
        // 持有用户比较函数的副本。
        bool operator()(const Cursor& left, const Cursor& right) const {
        // 判断 left 是否应该排在 right 之后。
            if (compare(left.row, right.row)) return false;
            // 如果 left 严格小于 right，那它显然不该排后面。
            if (compare(right.row, left.row)) return true;
            // 如果 right 严格小于 left，那 left 就该排后面。
            return left.run > right.run || (left.run == right.run && left.sequence > right.sequence);
            // 两者相等时退化到段号与段内序号比较，保证归并结果稳定。
        }
        // 比较运算结束。
    };
    // 比较器结构结束。
    std::vector<std::ifstream> inputs;
    // 每个归并段对应一个输入流。
    inputs.reserve(runs.size());
    // 预留容量，避免边读边扩容。
    for (std::size_t index = 0; index < runs.size(); ++index) {
    // 逐段做归并前的校验。
        const auto metadata = detail::readMetadata(metadataFiles[index]);
        // 先读出该段的元数据。
        if (metadata.value("operationId", "") != operationId || metadata.value("runIndex", std::size_t{1}) != index ||
        // 四项校验：操作标识、段序号、校验算法，以及必备字段是否齐全。
            metadata.value("checksumAlgorithm", "") != "fnv1a64" || !metadata.contains("rows") || !metadata.contains("checksum"))
            // @
            throw MiniSqlError(ErrorCode::Storage, "External sort metadata mismatch");
            // 不匹配就不用这份结果，直接按存储错误抛出。
        const auto expectedRows = metadata.at("rows").get<std::size_t>();
        // 取出元数据里记录的行数。
        const auto expectedChecksum = metadata.at("checksum").get<std::string>();
        // 取出元数据里记录的校验和。
        std::ifstream verification(runs[index], std::ios::binary);
        // 重新打开段文件，逐行重算校验和。
        if (!verification) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort run");
        // 打开失败按存储错误抛出。
        std::uint64_t checksum = detail::checksumBytes("");
        // 校验和累加器。
        std::size_t actualRows = 0;
        // 实际读到的行数。
        std::string line;
        // 行缓冲。
        while (std::getline(verification, line)) {
        // 逐行读取。
            if (checkCancelled) checkCancelled();
            // 每行之前检查取消。
            checksum = detail::checksumBytes(line, checksum);
            // 累加行内容。
            checksum = detail::checksumBytes("\n", checksum);
            // 累加换行符。
            ++actualRows;
            // 行数加一。
        }
        // 读完一段。
        if (actualRows != expectedRows || detail::checksumText(checksum) != expectedChecksum)
        // 行数或校验和对不上，说明段文件已损坏，拒绝继续。
            throw MiniSqlError(ErrorCode::Storage, "External sort run checksum mismatch");
        inputs.emplace_back(runs[index], std::ios::binary);
        // 校验通过，正式打开该段用于归并。
        if (!inputs.back()) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort run");
        // 再次确认流可用。
    }
    std::priority_queue<Cursor, std::vector<Cursor>, Later> queue(Later{compare});
    // 多路归并的小顶堆（用反向比较器实现最小优先）。
    for (std::size_t index = 0; index < inputs.size(); ++index) {
    // 逐段预读第一行。
        std::string line;
        // 行缓冲。
        if (std::getline(inputs[index], line) && !line.empty()) queue.push({nlohmann::json::parse(line), index, 0});
        // 读到非空行就压入堆，游标序号从 0 开始。
    }
    std::vector<Row> merged;
    // 归并结果容器。
    merged.reserve(rows.size());
    // 预留容量，避免反复扩容。
    while (!queue.empty()) {
    // 反复取堆顶的最小元素。
        if (checkCancelled) checkCancelled();
        // 每次取之前检查取消。
        Cursor cursor = queue.top();
        // 取出当前最小行。
        queue.pop();
        // 弹出堆顶。
        merged.push_back(std::move(cursor.row));
        // 追加到结果里，用 move 避免复制。
        std::string line;
        // 行缓冲。
        if (std::getline(inputs[cursor.run], line) && !line.empty())
        // 从该行所属的段继续读下一行。
            queue.push({nlohmann::json::parse(line), cursor.run, cursor.sequence + 1});
    }
    // 归并循环结束。
    rows = std::move(merged);
    // 用归并结果替换原容器；返回后临时文件由清理器删除。
}
// 外部排序函数结束。
}
