#pragma once
#include <filesystem>
#include <cstdint>

namespace minisql::storage {
class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const std::filesystem::path& path);
    ~ExclusiveFileLock();
    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
private:
    std::intptr_t handle_ = -1;
};
void syncFile(const std::filesystem::path& path);
void publishFile(const std::filesystem::path& source, const std::filesystem::path& destination);
}
