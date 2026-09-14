#pragma once
#include <filesystem>
#include <cstdint>

namespace minisql::storage {
class ExclusiveFileLock {
// 独占文件锁：保证同一个数据库文件不会被两个进程同时打开。
public:
    explicit ExclusiveFileLock(const std::filesystem::path& path);
    // 构造即加锁；拿不到锁会抛错。explicit 禁止隐式转换。
    ~ExclusiveFileLock();
    // 析构即解锁，因此可以用作用域自动管理锁的生命周期。
    ExclusiveFileLock(const ExclusiveFileLock&) = delete;
    // 禁止拷贝：锁不能被复制成两把。
    ExclusiveFileLock& operator=(const ExclusiveFileLock&) = delete;
    // 同样禁止赋值。
private:
    std::intptr_t handle_ = -1;
    // 底层文件句柄；-1 表示尚未加锁或已释放。
};
void syncFile(const std::filesystem::path& path);
// 把文件内容真正刷到磁盘：防止掉电后已提交的数据只停留在系统缓存里。
void publishFile(const std::filesystem::path& source, const std::filesystem::path& destination);
// 原子发布：把临时文件改名成正式文件，保证外部要么看到旧文件、要么看到新文件。
}
