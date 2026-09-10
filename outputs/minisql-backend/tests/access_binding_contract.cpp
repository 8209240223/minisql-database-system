#include "minisql/execution/database.hpp"
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::filesystem::path testPath() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("minisql-access-binding-" + std::to_string(stamp) + ".pages");
}

void removeArtifacts(const std::filesystem::path& path) {
    std::error_code error;
    for (const auto& suffix : {"", ".wal", ".ckpt", ".lock"})
        std::filesystem::remove(path.string() + suffix, error);
}
}

int main() {
    const auto path = testPath();
    removeArtifacts(path);
    {
        minisql::execution::Database database(path);
        require(database.execute("CREATE TABLE Public_Records(id INT); CREATE TABLE Secret_Records(id INT);")["success"] == true,
            "catalog binding fixture");
        const auto joined = database.resolveAccessObjects(
            "SELECT p.id FROM PUBLIC_RECORDS AS p JOIN secret_records AS s ON p.id=s.id;");
        require(joined == std::vector<std::string>{"public_records", "secret_records"},
            "catalog resolves physical tables and excludes aliases");
        const auto derived = database.resolveAccessObjects(
            "SELECT * FROM (SELECT id FROM SECRET_RECORDS) AS hidden;");
        require(derived == std::vector<std::string>{"secret_records"},
            "catalog resolves derived-table source and excludes derived alias");
        const auto nested = database.resolveAccessObjects(
            "SELECT * FROM public_records WHERE id IN (SELECT id FROM secret_records);");
        require(nested == std::vector<std::string>{"public_records", "secret_records"},
            "catalog resolves nested subquery sources");
        const auto cte = database.resolveAccessObjects(
            "WITH visible AS (SELECT id FROM SECRET_RECORDS) SELECT * FROM visible;");
        require(cte == std::vector<std::string>{"secret_records"},
            "catalog resolves CTE source after parser fallback and excludes CTE alias");
        const auto namedCte = database.resolveAccessObjects(
            "WITH visible(id) AS (SELECT id FROM SECRET_RECORDS) SELECT id FROM visible;");
        require(namedCte == std::vector<std::string>{"secret_records"},
            "catalog resolves named-column CTE source and excludes its alias");
        const auto unsupported = database.resolveAccessObjects(
            "WITH visible AS (SELECT id FROM PUBLIC_RECORDS) SELECT * FROM visible JOIN SECRET_RECORDS ON visible.id = SECRET_RECORDS.id;");
        require(unsupported == std::vector<std::string>{"public_records", "secret_records"},
            "catalog binds unsupported CTE query sources conservatively");
    }
    removeArtifacts(path);
    return 0;
}
