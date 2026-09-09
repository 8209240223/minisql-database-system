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
    if (value.empty()) return "local";
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '-' && character != '_') character = '-';
    }
    return value;
}
inline std::uint64_t checksumBytes(std::string_view bytes, std::uint64_t hash = 1469598103934665603ULL) {
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
inline std::string checksumText(std::uint64_t checksum) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << checksum;
    return output.str();
}
inline nlohmann::json readMetadata(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort metadata");
    try {
        return nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
        throw MiniSqlError(ErrorCode::Storage, "Malformed external sort metadata");
    }
}
}

template <typename Rows, typename Compare>
void externalSort(Rows& rows, Compare compare, std::size_t maxRows,
                  const std::filesystem::path& directory,
                  const std::string& operationId = "local",
                  const std::function<void()>& checkCancelled = {}) {
    using Row = typename Rows::value_type;
    if (maxRows == 0) throw MiniSqlError(ErrorCode::InvalidArgument, "External sort row budget must be positive");
    if (rows.size() <= maxRows) {
        std::stable_sort(rows.begin(), rows.end(), compare);
        return;
    }
    std::filesystem::create_directories(directory);
    std::vector<std::filesystem::path> runs;
    std::vector<std::filesystem::path> metadataFiles;
    struct ArtifactCleanup {
        std::vector<std::filesystem::path>& runs;
        std::vector<std::filesystem::path>& metadata;
        ~ArtifactCleanup() {
            for (const auto& file : runs) {
                std::error_code error;
                std::filesystem::remove(file, error);
            }
            for (const auto& file : metadata) {
                std::error_code error;
                std::filesystem::remove(file, error);
            }
        }
    } cleanup{runs, metadataFiles};
    const auto prefix = "sort-" + detail::sortArtifactId(operationId) + "-";
    for (std::size_t begin = 0; begin < rows.size(); begin += maxRows) {
        if (checkCancelled) checkCancelled();
        const auto end = std::min(rows.size(), begin + maxRows);
        std::stable_sort(rows.begin() + static_cast<std::ptrdiff_t>(begin), rows.begin() + static_cast<std::ptrdiff_t>(end), compare);
        const auto run = directory / (prefix + std::to_string(runs.size()) + ".jsonl");
        std::ofstream output(run, std::ios::binary | std::ios::trunc);
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort run");
        std::uint64_t checksum = detail::checksumBytes("");
        std::size_t runRows = 0;
        for (std::size_t index = begin; index < end; ++index) {
            const auto line = rows[index].dump();
            output << line << '\n';
            checksum = detail::checksumBytes(line, checksum);
            checksum = detail::checksumBytes("\n", checksum);
            ++runRows;
        }
        if (!output) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort run");
        const auto metadataFile = std::filesystem::path(run.string() + ".meta.json");
        std::ofstream metadataOutput(metadataFile, std::ios::binary | std::ios::trunc);
        if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot create external sort metadata");
        metadataOutput << nlohmann::json{{"operationId", operationId}, {"runIndex", runs.size()}, {"rows", runRows},
            {"checksumAlgorithm", "fnv1a64"}, {"checksum", detail::checksumText(checksum)}}.dump() << '\n';
        if (!metadataOutput) throw MiniSqlError(ErrorCode::Storage, "Cannot write external sort metadata");
        runs.push_back(run);
        metadataFiles.push_back(metadataFile);
    }
    struct Cursor { nlohmann::json row; std::size_t run; std::size_t sequence; };
    struct Later {
        Compare compare;
        bool operator()(const Cursor& left, const Cursor& right) const {
            if (compare(left.row, right.row)) return false;
            if (compare(right.row, left.row)) return true;
            return left.run > right.run || (left.run == right.run && left.sequence > right.sequence);
        }
    };
    std::vector<std::ifstream> inputs;
    inputs.reserve(runs.size());
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto metadata = detail::readMetadata(metadataFiles[index]);
        if (metadata.value("operationId", "") != operationId || metadata.value("runIndex", std::size_t{1}) != index ||
            metadata.value("checksumAlgorithm", "") != "fnv1a64" || !metadata.contains("rows") || !metadata.contains("checksum"))
            throw MiniSqlError(ErrorCode::Storage, "External sort metadata mismatch");
        const auto expectedRows = metadata.at("rows").get<std::size_t>();
        const auto expectedChecksum = metadata.at("checksum").get<std::string>();
        std::ifstream verification(runs[index], std::ios::binary);
        if (!verification) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort run");
        std::uint64_t checksum = detail::checksumBytes("");
        std::size_t actualRows = 0;
        std::string line;
        while (std::getline(verification, line)) {
            if (checkCancelled) checkCancelled();
            checksum = detail::checksumBytes(line, checksum);
            checksum = detail::checksumBytes("\n", checksum);
            ++actualRows;
        }
        if (actualRows != expectedRows || detail::checksumText(checksum) != expectedChecksum)
            throw MiniSqlError(ErrorCode::Storage, "External sort run checksum mismatch");
        inputs.emplace_back(runs[index], std::ios::binary);
        if (!inputs.back()) throw MiniSqlError(ErrorCode::Storage, "Cannot read external sort run");
    }
    std::priority_queue<Cursor, std::vector<Cursor>, Later> queue(Later{compare});
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        std::string line;
        if (std::getline(inputs[index], line) && !line.empty()) queue.push({nlohmann::json::parse(line), index, 0});
    }
    std::vector<Row> merged;
    merged.reserve(rows.size());
    while (!queue.empty()) {
        if (checkCancelled) checkCancelled();
        Cursor cursor = queue.top();
        queue.pop();
        merged.push_back(std::move(cursor.row));
        std::string line;
        if (std::getline(inputs[cursor.run], line) && !line.empty())
            queue.push({nlohmann::json::parse(line), cursor.run, cursor.sequence + 1});
    }
    rows = std::move(merged);
}
}
