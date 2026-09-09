#include "minisql/storage/file_io.hpp"
#include "minisql/common/error.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace minisql::storage {
namespace {
[[noreturn]] void fail(const char* message) { throw MiniSqlError(ErrorCode::Storage, message); }
}
ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("Database lock unavailable: database may already be open");
    handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    const auto handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (handle < 0) fail("Cannot open database lock");
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) { ::close(handle); fail("Database lock unavailable: database may already be open"); }
    handle_ = handle;
#endif
}
ExclusiveFileLock::~ExclusiveFileLock() {
#ifdef _WIN32
    if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    if (handle_ != -1) ::close(static_cast<int>(handle_));
#endif
}
void syncFile(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) fail("Cannot open file for durable flush");
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
#else
    const auto handle = ::open(path.c_str(), O_RDWR);
    if (handle < 0) fail("Cannot open file for durable flush");
    const bool ok = ::fsync(handle) == 0;
    ::close(handle);
#endif
    if (!ok) fail("Durable file flush failed");
}
void publishFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail("Atomic file publication failed");
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) fail("Atomic file publication failed");
    const auto directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) fail("Cannot open directory for sync");
    const bool ok = ::fsync(directory) == 0;
    ::close(directory);
    if (!ok) fail("Directory sync failed");
#endif
    syncFile(destination);
}
}
