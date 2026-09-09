#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include "minisql/common/config.hpp"

namespace minisql::test {
inline Environment emptyEnvironment() {
    return [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
}
inline std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
class TempDirectory {
public:
    TempDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::current_path() / ("minisql-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(path_)) return;
        }
        throw std::runtime_error("Cannot create unique test directory");
    }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec); // Only this fixture's uniquely created directory.
    }
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path write(const std::string& name, const std::string& content) const {
        auto file = path_ / pathFromUtf8(name);
        std::ofstream output(file, std::ios::binary);
        output << content;
        if (!output) throw std::runtime_error("Cannot write test fixture");
        return file;
    }
private:
    std::filesystem::path path_;
};
inline std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read test output");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
} // namespace minisql::test
