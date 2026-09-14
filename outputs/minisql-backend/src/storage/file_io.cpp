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
// 统一的存储错误出口。
}
ExclusiveFileLock::ExclusiveFileLock(const std::filesystem::path& path) {
// 加锁：打开锁文件并独占它。
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    // 第三个参数是共享模式，传 0 表示不允许其他进程同时打开，即独占。
    if (handle == INVALID_HANDLE_VALUE) fail("Database lock unavailable: database may already be open");
    // 打开失败通常意味着数据库已被另一个进程占用。
    handle_ = reinterpret_cast<std::intptr_t>(handle);
    // 把句柄保存到成员里，供析构时释放。
#else
    const auto handle = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    // 以读写方式打开，不存在则创建，权限为仅所有者可读写。
    if (handle < 0) fail("Cannot open database lock");
    // 打开失败说明路径或权限有问题。
    if (::flock(handle, LOCK_EX | LOCK_NB) != 0) { ::close(handle); fail("Database lock unavailable: database may already be open"); }
    // 尝试非阻塞独占锁；失败要先把文件描述符关掉再报错，避免泄漏。
    handle_ = handle;
    // 保存描述符。
#endif
}
ExclusiveFileLock::~ExclusiveFileLock() {
// 析构：释放锁。
#ifdef _WIN32
    if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
    // 关闭句柄即释放独占。
#else
    if (handle_ != -1) ::close(static_cast<int>(handle_));
    // 关闭描述符即释放 flock。
#endif
}
void syncFile(const std::filesystem::path& path) {
// 把文件缓冲刷到物理磁盘。
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    // 以只写方式打开已存在的文件，同时允许别人读写或删除，避免影响正常使用。
    if (handle == INVALID_HANDLE_VALUE) fail("Cannot open file for durable flush");
    // 打不开就无法保证持久化，直接报错。
    const bool ok = FlushFileBuffers(handle) != 0;
    // 触发系统把该文件的缓冲写到磁盘。
    CloseHandle(handle);
    // 刷完立即关闭句柄。
#else
    const auto handle = ::open(path.c_str(), O_RDWR);
    // 打开文件。
    if (handle < 0) fail("Cannot open file for durable flush");
    // 打不开就报错。
    const bool ok = ::fsync(handle) == 0;
    // fsync 阻塞到数据真正落盘。
    ::close(handle);
    // 关闭描述符。
#endif
    if (!ok) fail("Durable file flush failed");
    // 刷盘失败同样要暴露出来，不能静默继续。
}
void publishFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
// 原子发布：把临时文件改名成正式文件，再把结果刷到磁盘。
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    // 带“替换已存在文件”与“写透”两个标志的改名，语义上等价于原子替换。
        fail("Atomic file publication failed");
#else
    std::error_code error;
    // 用错误码版本，避免抛标准异常。
    std::filesystem::rename(source, destination, error);
    // 同一文件系统内的改名是原子的。
    if (error) fail("Atomic file publication failed");
    // 改名失败说明跨文件系统或权限不足。
    const auto directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    // 打开父目录，为了把“改名”这个元数据变更也刷盘。
    if (directory < 0) fail("Cannot open directory for sync");
    // 打不开目录就报错。
    const bool ok = ::fsync(directory) == 0;
    // 刷目录项，保证掉电后改名本身不丢。
    ::close(directory);
    // 关闭目录描述符。
    if (!ok) fail("Directory sync failed");
    // 刷目录失败要报错。
#endif
    syncFile(destination);
    // 最后再刷一次文件内容本身。
}
}
