#include "minisql/storage/bplus_tree.hpp"
#include <iostream>
#include <stdexcept>
using namespace minisql;
using namespace minisql::storage;
int checks = 0;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); ++checks; }
RowRef row(std::uint64_t id) { return {{id, 1}, {static_cast<std::uint16_t>(id), 1}}; }
int main() {
    BPlusTree tree(4, true);
    for (int value = 0; value < 100; ++value) require(tree.insert({{std::int32_t(value)}}, row(value)), "insert unique key");
    require(tree.size() == 100, "tree size");
    require(tree.height() >= 2, "tree grows beyond one leaf");
    require(tree.validate(), "tree structure valid");
    require(tree.search({{std::int32_t(42)}}).size() == 1, "point search");
    require(!tree.insert({{std::int32_t(42)}}, row(999)), "duplicate key rejected");
    const auto range = tree.range(IndexKey{{std::int32_t(20)}}, true, IndexKey{{std::int32_t(29)}}, true);
    require(range.size() == 10, "inclusive range size");
    require(range.front().page.id == 20 && range.back().page.id == 29, "range order and bounds");
    const auto openRange = tree.range(IndexKey{{std::int32_t(20)}}, false, IndexKey{{std::int32_t(29)}}, false);
    require(openRange.size() == 8, "exclusive range size");
    BPlusTree composite(4, true);
    require(composite.insert({{std::int32_t(1), std::string("a")}}, row(1)), "composite first");
    require(composite.insert({{std::int32_t(1), std::string("b")}}, row(2)), "composite second");
    require(composite.insert({{std::int32_t(2), std::string("a")}}, row(3)), "composite third");
    require(composite.search({{std::int32_t(1), std::string("b")}}).front().page.id == 2, "composite search");
    require(composite.validate(), "composite tree valid");
    std::cout << checks << " B+ tree checks passed\n";
}