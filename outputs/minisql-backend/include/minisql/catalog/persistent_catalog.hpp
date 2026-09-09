#pragma once
#include "minisql/catalog/catalog.hpp"
#include "minisql/storage/heap.hpp"

namespace minisql::catalog {
struct StoredTable { std::int32_t id; sql::Statement definition; };
class PersistentCatalog {
public:
    explicit PersistentCatalog(storage::HeapStore& heap);
    const Catalog& view() const { return view_; }
    const std::vector<StoredTable>& tables() const { return tables_; }
    std::int32_t create(const sql::Statement& definition);
    void createIndex(const sql::Statement& definition);
    void dropIndex(const sql::Statement& definition);
    void reload();
private:
    storage::HeapStore& heap_;
    Catalog view_;
    std::vector<StoredTable> tables_;
    std::int32_t nextId_ = 2;
};
}
