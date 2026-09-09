#include "minisql/storage/page_bplus_tree.hpp"
#include "minisql/common/error.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace minisql;
using namespace minisql::storage;

int checks = 0;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
RowRef row(std::uint64_t id) { return {{id, 1}, {static_cast<std::uint16_t>(id), 1}}; }
IndexKey intKey(std::int32_t value) { return {{value}}; }

namespace {
constexpr std::uint64_t owner = 42;
const std::size_t maxKeys = 4;
}

int mainImpl(int argc, char** argv);
int main(int argc, char** argv) {
    try {
    return mainImpl(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "FAIL(" << checks << "): " << e.what() << "\n";
        return 1;
    }
}
int mainImpl(int argc, char** argv) {
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("pagebplus-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "pages.db";

    // -- 空树 --
    std::size_t persistedSize = 0;
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 4, ReplacementPolicy::LRU);
        PageBPlusTree tree(file, buffer, owner, maxKeys, true);
        require(!tree.exists(), "empty tree has no meta yet");
        require(tree.size() == 0, "empty tree size");
        require(tree.search(intKey(1)).empty(), "empty tree search");
        require(tree.validate(), "empty tree valid");
        require(tree.create(), "first create");
        require(!tree.create(), "second create is no-op");

        // 插入 120 个 int 键 → 必然触发叶/内节点/根分裂
        for (std::int32_t i = 0; i < 120; ++i) require(tree.insert(intKey(i), row(i)), "insert unique through splits");
        require(tree.size() == 120, "size after bulk insert");
        require(tree.height() >= 2, "tree grows beyond one leaf");
        require(tree.pageCount() >= 2, "index uses more than one page");
        require(tree.validate(), "tree structure valid after splits");

        require(tree.insert(intKey(42), row(999)) == false, "duplicate key rejected");
        require(tree.search(intKey(42)).size() == 1 && tree.search(intKey(42)).front().page.id == 42, "point search");
        require(tree.search(intKey(119)).front().page.id == 119, "last key search");

        const auto rangeClosed = tree.range(intKey(20), true, intKey(29), true);
        require(rangeClosed.size() == 10, "inclusive range size");
        require(rangeClosed.front().page.id == 20 && rangeClosed.back().page.id == 29, "inclusive range order");
        const auto rangeOpen = tree.range(intKey(20), false, intKey(29), false);
        require(rangeOpen.size() == 8, "exclusive range size");
        const auto openEnd = tree.range(intKey(20), true, std::nullopt, true);
        require(openEnd.size() == 100, "lower-bounded open-ended range");

        // 复合键
        {
            std::filesystem::path compositePath = directory / "composite.db";
            auto cfile = std::make_shared<PageFile>(compositePath);
            BufferPool cbuffer(cfile, 4, ReplacementPolicy::LRU);
            PageBPlusTree composite(cfile, cbuffer, owner, maxKeys, true);
            require(composite.create(), "composite create");
            require(composite.insert({{std::int32_t(1), std::string("a")}}, row(1)), "composite first");
            require(composite.insert({{std::int32_t(1), std::string("b")}}, row(2)), "composite second");
            require(composite.insert({{std::int32_t(2), std::string("a")}}, row(3)), "composite third");
            require(composite.search({{std::int32_t(1), std::string("b")}}).front().page.id == 2, "composite search");
            require(composite.insert({{std::int32_t(1), std::string("b")}}, row(99)) == false, "composite duplicate");
            require(composite.validate(), "composite tree valid");
        }

        persistedSize = tree.size();
        buffer.flushAll();
    }

    // -- 重启重开：重新打开同一页文件，验证叶链/根页可恢复 --
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 4, ReplacementPolicy::LRU);
        PageBPlusTree tree(file, buffer, owner, maxKeys, true);
        require(tree.exists(), "meta survives restart");
        require(tree.size() == persistedSize && persistedSize == 120, "size survives restart");
        require(tree.search(intKey(42)).front().page.id == 42, "search survives restart");
        const auto rangeClosed = tree.range(intKey(20), true, intKey(29), true);
        require(rangeClosed.size() == 10 && rangeClosed.front().page.id == 20, "range survives restart via leaf chain");
        require(tree.validate(), "structure survives restart");
    }

    // -- 删除：借位、合并与根收缩 --
    {
        std::filesystem::path delPath = directory / "delete.db";
        auto df = std::make_shared<PageFile>(delPath);
        BufferPool dbuf(df, 4, ReplacementPolicy::LRU);
        PageBPlusTree del(df, dbuf, owner, maxKeys, true);
        require(del.create(), "delete create");
        // 插入 19 个键（maxKeys=4，minKeys=2）会触发叶/内节点/根分裂成多级树
        for (std::int32_t i = 0; i < 19; ++i) require(del.insert(intKey(i), row(i)), "delete insert");
        require(del.size() == 19 && del.height() >= 2, "delete tree multi-level");
        require(del.validate(), "delete tree valid before removal");

        // 逐个删除，触发不同位置（收尾/中段/借位/合并）的分支
        for (std::int32_t i = 18; i >= 0; --i) {
            require(del.erase(intKey(i), row(i)), "erase present key");
            require(del.search(intKey(i)).empty(), "erased key gone");
            require(del.size() == static_cast<std::size_t>(i), "size tracks deletion");
            require(del.validate(), "structure after each erase");
        }
        require(del.erase(intKey(3), row(3)) == false, "erase absent key");
        require(del.size() == 0, "delete down to empty");
        require(del.height() == 1, "tree shrunk to empty leaf root");
        require(del.validate(), "empty tree valid");
    }

    // -- 删除后部分保留 + 重启恢复 --
    {
        std::filesystem::path keepPath = directory / "keep.db";
        auto kf = std::make_shared<PageFile>(keepPath);
        BufferPool kbuf(kf, 4, ReplacementPolicy::LRU);
        PageBPlusTree keep(kf, kbuf, owner, maxKeys, true);
        require(keep.create(), "keep create");
        for (std::int32_t i = 0; i < 40; ++i) require(keep.insert(intKey(i), row(i)), "keep insert");
        for (std::int32_t i = 0; i < 40; i += 2) require(keep.erase(intKey(i), row(i)), "erase even keys");
        require(keep.size() == 20, "kept 20 odd keys");
        require(keep.validate(), "keep tree valid");
        kbuf.flushAll();
    }
    {
        std::filesystem::path keepPath2 = directory / "keep.db";
        auto kf2 = std::make_shared<PageFile>(keepPath2);
        BufferPool kbuf2(kf2, 4, ReplacementPolicy::LRU);
        PageBPlusTree keep2(kf2, kbuf2, owner, maxKeys, true);
        require(keep2.exists(), "keep meta survives restart");
        require(keep2.size() == 20, "keep size survives restart");
        require(keep2.search(intKey(1)).front().page.id == 1, "odd key searchable after restart");
        require(keep2.search(intKey(30)).empty(), "even key absent after restart");
        require(keep2.range(intKey(10), true, intKey(20), true).size() == 5, "range over kept keys");
        require(keep2.validate(), "keep structure survives restart via leaf chain");
    }

    // -- 页级结构校验 inspect() --
    {
        std::filesystem::path inspPath = directory / "inspect.db";
        auto inf = std::make_shared<PageFile>(inspPath);
        BufferPool inbuf(inf, 4, ReplacementPolicy::LRU);
        PageBPlusTree ins(inf, inbuf, owner, maxKeys, true);
        require(!ins.inspect().present, "inspect absent on empty");
        require(ins.create(), "inspect create");
        {
            const auto before = ins.inspect();
            require(before.present && before.rootReachable && before.height == 0 && before.rowCount == 0,
                "empty inspect reports empty valid root");
        }
        for (std::int32_t i = 0; i < 120; ++i) require(ins.insert(intKey(i), row(i)), "inspect insert");
        for (std::int32_t i = 1; i < 120; i += 3) require(ins.erase(intKey(i), row(i)), "inspect erase");

        const auto diag = ins.inspect();
        require(diag.present, "inspect present after build");
        require(diag.height == ins.height() && diag.height >= 2, "inspect height matches tree");
        require(diag.nodeCount == ins.pageCount() - 1, "node pages == owner pages minus meta");
        require(diag.leafCount >= 2, "inspect reports multiple leaves");
        require(diag.leafChainLength == diag.leafCount, "inspect leaf chain length matches leaf count");
        require(diag.rowCount == ins.size(), "inspect row count matches size");
        require(diag.rootReachable && diag.parentLinksValid && diag.leafChainLinked, "inspect flags all valid");
        require(diag.problems.empty(), "inspect reports no structural problems");
        require(diag.pages.size() == diag.nodeCount, "inspect enumerates one entry per node");
        // 校验每页记录与现状一致：叶节点 height 必须为 0；内节点必有子页
        bool anyLeafMislabeled = false;
        for (const auto& info : diag.pages) if (info.leaf && info.height != 0) anyLeafMislabeled = true;
        require(!anyLeafMislabeled, "inspect leaf height is zero");
    }

    // -- 损坏拒绝：错误代次访问节点页应明确失败 --
    {
        auto file = std::make_shared<PageFile>(path);
        BufferPool buffer(file, 4, ReplacementPolicy::LRU);
        PageBPlusTree tree(file, buffer, owner, maxKeys, true);
        require(tree.exists(), "corruption probe sees meta");
        bool staleCaught = false;
        try {
            const auto pages = file->pagesFor(owner);
            require(!pages.empty(), "owner has pages");
            // 用一个必然不匹配的代次去读节点页，应抛 STALE_PAGE_ID 类错误
            for (const auto& ref : pages) (void)buffer.get({ref.id, ref.generation + 1000000});
        } catch (const MiniSqlError&) {
            staleCaught = true;
        }
        require(staleCaught, "stale page reference must be rejected");
    }

    std::cout << checks << " page-level B+ tree checks passed\n";
    return 0;
}