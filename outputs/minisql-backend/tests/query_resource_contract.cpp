#include "minisql/execution/executor.hpp"
#include "minisql/common/error.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using json = nlohmann::json;
using minisql::execution::RowStream;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class VectorStream final : public RowStream {
public:
    explicit VectorStream(std::vector<json> rows) : rows_(std::move(rows)) {}
    bool next(json& row) override {
        if (cancelled_) throw minisql::MiniSqlError(minisql::ErrorCode::Cancelled, "cancelled");
        if (cursor_ >= rows_.size()) return false;
        row = rows_[cursor_++];
        return true;
    }
    void cancel() override { cancelled_ = true; }
    void close() override { closed_ = true; }
    json resourceUsage() const override {
        return {{"kind", "VectorStream"}, {"rows", cursor_}, {"closed", closed_}};
    }
private:
    std::vector<json> rows_;
    std::size_t cursor_ = 0;
    bool cancelled_ = false;
    bool closed_ = false;
};

std::vector<json> rows(std::initializer_list<int> values) {
    std::vector<json> result;
    for (const auto value : values) result.push_back(json::array({value}));
    return result;
}

std::vector<json> drain(RowStream& stream) {
    std::vector<json> result;
    json row;
    while (stream.next(row)) result.push_back(row);
    return result;
}

bool emptyDirectory(const std::filesystem::path& directory) {
    return !std::filesystem::exists(directory) || std::filesystem::directory_iterator(directory) == std::filesystem::directory_iterator{};
}
}

int main() {
    using namespace minisql::execution;
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() / ("minisql-query-resource-" + suffix);
    std::filesystem::create_directories(directory);
    try {
        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 1024 * 1024);
            ExternalSortRowStream stream(std::make_unique<VectorStream>(rows({9, 1, 7, 3, 5, 2, 8, 4, 6, 0})),
                [](const json& left, const json& right) { return left < right; }, resources, directory, "sort", 2);
            const auto actual = drain(stream);
            stream.close();
            require(actual == rows({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), "external sort output");
            const auto usage = stream.resourceUsage();
            require(usage.at("external") == true, "sort must spill");
            require(usage.at("spillFiles").get<std::size_t>() > 0, "sort spill files");
            require(usage.at("spillBytes").get<std::uint64_t>() > 0, "sort spill bytes");
            require(usage.at("memoryPeakBytes").get<std::size_t>() <= 1024, "sort memory budget");
            require(usage.at("tempDiskCurrentBytes") == 0, "sort disk released");
            require(usage.at("tempFilesCleaned") == usage.at("spillFiles"), "sort files cleaned");
        }
        require(emptyDirectory(directory), "sort left temporary files");

        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 1024 * 1024);
            auto sorted = std::make_unique<ExternalSortRowStream>(
                std::make_unique<VectorStream>(rows({3, 1, 3, 2, 1, 2, 3})),
                [](const json& left, const json& right) { return left < right; }, resources, directory, "distinct", 2);
            DistinctRowStream stream(std::move(sorted), resources);
            const auto actual = drain(stream);
            stream.close();
            require(actual == rows({1, 2, 3}), "distinct output");
            require(stream.resourceUsage().at("duplicates") == 4, "distinct duplicate count");
        }
        require(emptyDirectory(directory), "distinct left temporary files");

        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 1024 * 1024);
            std::vector<json> records{
                {{"key", json::array({2})}, {"values", json::array({5})}},
                {{"key", json::array({1})}, {"values", json::array({3})}},
                {{"key", json::array({2})}, {"values", json::array({7})}},
                {{"key", json::array({1})}, {"values", json::array({4})}}};
            auto sorted = std::make_unique<ExternalSortRowStream>(std::make_unique<VectorStream>(std::move(records)),
                [](const json& left, const json& right) { return left.at("key") < right.at("key"); },
                resources, directory, "aggregate", 1);
            GroupedAggregateRowStream stream(std::move(sorted), json::array({0}),
                [](json& state, const json& values) { state[0] = state[0].get<int>() + values[0].get<int>(); },
                [](const json& key, const json& state) { return json::array({key[0], state[0]}); }, resources);
            const auto actual = drain(stream);
            stream.close();
            require(actual == std::vector<json>{json::array({1, 7}), json::array({2, 12})}, "aggregate output");
            require(stream.resourceUsage().at("spillFiles").get<std::size_t>() > 0, "aggregate must spill");
        }
        require(emptyDirectory(directory), "aggregate left temporary files");

        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 1024 * 1024);
            JoinRowStream stream(std::make_unique<VectorStream>(rows({1, 2, 4})),
                std::make_unique<VectorStream>(rows({2, 2, 3, 4})),
                [](const json& row) { return row[0] == row[1]; }, resources, directory, "join", 2, 1, false,
                std::pair<std::size_t, std::size_t>{0, 0});
            const auto actual = drain(stream);
            stream.close();
            require(actual == std::vector<json>{json::array({2, 2}), json::array({2, 2}), json::array({4, 4})}, "join output");
            require(stream.resourceUsage().at("external") == true, "join must spill");
        }
        require(emptyDirectory(directory), "join left temporary files");

        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 8);
            try {
                ExternalSortRowStream stream(std::make_unique<VectorStream>(rows({5, 4, 3, 2, 1})),
                    [](const json& left, const json& right) { return left < right; }, resources, directory, "disk-limit", 1);
                json row;
                (void)stream.next(row);
                require(false, "disk limit must fail");
            } catch (const minisql::MiniSqlError& error) {
                require(error.code() == minisql::ErrorCode::Execution, "disk limit error code");
                require(std::string(error.what()).find("Temporary disk budget exceeded") != std::string::npos, "disk limit message");
            }
        }
        require(emptyDirectory(directory), "disk limit left temporary files");

        {
            auto resources = std::make_shared<QueryResourceManager>(1024, 1024 * 1024);
            std::size_t checks = 0;
            try {
                ExternalSortRowStream stream(std::make_unique<VectorStream>(rows({9, 8, 7, 6, 5, 4, 3, 2, 1})),
                    [](const json& left, const json& right) { return left < right; }, resources, directory, "cancel", 1,
                    [&] { if (++checks == 5) throw minisql::MiniSqlError(minisql::ErrorCode::Cancelled, "cancelled"); });
                json row;
                (void)stream.next(row);
                require(false, "cancel must fail");
            } catch (const minisql::MiniSqlError& error) {
                require(error.code() == minisql::ErrorCode::Cancelled, "cancel error code");
            }
        }
        require(emptyDirectory(directory), "cancel left temporary files");
        std::filesystem::remove_all(directory);
        std::cout << "query resource contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code cleanupError;
        std::filesystem::remove_all(directory, cleanupError);
        std::cerr << "query resource contract failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::error_code cleanupError;
        std::filesystem::remove_all(directory, cleanupError);
        std::cerr << "query resource contract failed with an unknown error\n";
        return 1;
    }
}
