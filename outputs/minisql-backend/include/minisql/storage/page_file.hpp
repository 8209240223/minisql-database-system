#pragma once
#include "minisql/storage/page.hpp"
#include "minisql/storage/file_io.hpp"
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <functional>
#include <string_view>

namespace minisql::storage {
struct PageRef { PageId id; std::uint64_t generation; };
struct PageIoStats { std::uint64_t reads = 0, writes = 0, errors = 0; };
class PageFile {
public:
    using CommitObserver = std::function<void(std::string_view)>;
    explicit PageFile(const std::filesystem::path& path, CommitObserver observer = {});
    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    PageRef allocate(std::uint64_t owner);
    SlottedPage read(PageRef ref);
    void write(const SlottedPage& page);
    void release(PageRef ref);
    void flush();
    void beginWriteBatch();
    void rollbackWriteBatch();
    void commitWriteBatch();
    void checkpoint();
    void requireHealthy() const;
    bool writeBatchActive() const { return batch_ != nullptr; }
    bool hasStagedPage(PageId id) const { return batch_ && batch_->pages.contains(id); }
    std::size_t stagedPageCount() const { return batch_ ? batch_->pages.size() : 0; }
    std::size_t allocatedPages() const { return active_.size(); }
    std::uint64_t walBytes() const;
    std::uint64_t lastCommitWalBytes() const { return lastCommitWalBytes_; }
    const std::filesystem::path& path() const { return path_; }
    const PageIoStats& ioStats() const { return ioStats_; }
    void resetIoStats() { ioStats_ = {}; }
    std::vector<PageRef> pagesFor(std::uint64_t owner) const;
private:
    std::filesystem::path path_;
    std::unique_ptr<ExclusiveFileLock> lock_;
    std::fstream file_;
    std::array<std::uint64_t, 2> identity_{};
    CommitObserver observer_;
    std::uint64_t count_ = 1;
    std::uint64_t lastCommitWalBytes_ = 0;
    bool failed_ = false;
    PageIoStats ioStats_;
    std::unordered_map<PageId, std::uint64_t> active_;
    std::unordered_map<PageId, std::uint64_t> owners_;
    std::vector<PageRef> free_;
    struct WriteBatch {
        std::uint64_t count;
        std::unordered_map<PageId, std::uint64_t> active, owners;
        std::vector<PageRef> free;
        std::unordered_map<PageId, PageBytes> pages;
        bool published = false;
    };
    std::unique_ptr<WriteBatch> batch_;
    PageBytes readRaw(PageId id);
    void writeRaw(PageId id, const PageBytes& bytes);
    void writeDiskRaw(PageId id, const PageBytes& bytes);
    void ensureIdentity();
    void recoverJournal(bool recovering);
    void checkpointJournal();
    void notify(std::string_view stage);
    void writeHeader();
    void requireActive(PageRef ref) const;
};
}
