#include "minisql/execution/database.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
using json = nlohmann::json;
int checks = 0;
void require(bool value, const char* name) { if (!value) throw std::runtime_error(name); ++checks; }
int main() {
    const auto directory = std::filesystem::path("tests/artifacts") /
        ("database-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "database.pages";
    {
        minisql::execution::Database database(path, 2);
        require(database.execute("")["statements"] == 0, "empty SQL");
        const auto compiled = database.compile("CREATE TABLE transient(id INT); SELECT id FROM transient;");
        require(compiled["success"] == true, "compile snapshot");
        require(database.catalog()["tables"].empty(), "compile no catalog effects");
        auto result = database.execute("CREATE TABLE student(id INT,name VARCHAR,age INT);"
            "INSERT INTO student(id,name,age) VALUES(1,'Alice',20);"
            "INSERT INTO student(age,id,name) VALUES(17,2,'Bob');"
            "INSERT INTO student(id,name,age) VALUES(3,'Tom''s;book',21);"
            "SELECT id,name FROM student WHERE age>18;");
        require(result["success"] == true, "core batch succeeds");
        require(result["results"].size() == 5, "per statement results");
        require(result["results"][4]["rows"] == json::array({json::array({1,"Alice"}),json::array({3,"Tom's;book"})}), "query actual rows");
        result = database.execute("SELECT name,id,name FROM student WHERE NOT age=17 AND (id=1 OR id=3);");
        require(result["success"] == true, "boolean query succeeds");
        require(result["results"][0]["columns"] == json::array({"name","id","name"}), "duplicate projection names");
        require(result["results"][0]["rows"][0] == json::array({"Alice",1,"Alice"}), "projection order");
        result = database.execute("DELETE FROM student WHERE id=1; SELECT * FROM student;");
        require(result["results"][0]["affectedRows"] == 1, "actual delete count");
        require(result["results"][1]["rows"].size() == 2, "delete applied");
        result = database.execute("INSERT INTO student(id,name,age) VALUES(4,'D',22); SELECT missing FROM student;");
        require(result["success"] == false && result["completedStatements"] == 1, "partial batch commit reported");
        result = database.execute("SELECT * FROM student WHERE id=4;");
        require(result["results"][0]["rows"].size() == 1, "earlier statement remains committed");
        result = database.execute("INSERT INTO student(id,name,age) VALUES('bad',1,2);");
        require(result["success"] == false, "wrong types rejected");
        result = database.execute("SELECT name FROM student WHERE id=999;");
        require(result["results"][0]["rows"].empty() && result["results"][0]["columns"] == json::array({"name"}), "empty result preserves schema");
        require(database.compile("SELECT name FROM student;")["success"] == true, "compile uses real schema");
        result = database.execute("CREATE TABLE committed(id INT); @");
        require(result["success"] == false && result["completedStatements"] == 1, "lexical failure preserves prior statement");
        require(database.compile("SELECT * FROM committed;")["success"] == true, "table committed before lexical failure");
        result = database.execute("INSERT INTO committed(id) VALUES(1); /* unterminated");
        require(result["success"] == false && result["completedStatements"] == 1, "unterminated comment after commit");
        require(database.execute("SELECT * FROM committed;")["results"][0]["rows"] == json::array({json::array({1})}), "row committed before comment failure");
        result = database.execute("INSERT INTO committed(id) VALUES(2); SELECT * FROM committed");
        require(result["success"] == false && result["completedStatements"] == 1, "missing final semicolon preserves commit");
        result = database.execute("SELECT * FROM committed;\r\n@");
        require(result["error"]["line"] == 2 && result["error"]["column"] == 1, "streaming error absolute location");
        result = database.execute("SELECT * FROM committed; SELECT 'bad\nstring' FROM committed;");
        require(result["success"] == false && result["completedStatements"] == 1, "multiline string rejected after earlier result");
    }
    {
        minisql::execution::Database database(path, 2);
        auto result = database.execute("SELECT id,name FROM student;");
        require(result["success"] == true, "query after reopen");
        require(result["results"][0]["rows"].size() == 3, "rows persisted");
        require(database.catalog()["tables"][0]["columns"].size() == 3, "catalog persisted");
        result = database.execute("DELETE FROM student; SELECT id FROM student;");
        require(result["results"][0]["affectedRows"] == 3, "delete all count");
        require(result["results"][1]["rows"].empty(), "delete all empty");
    }
    {
        minisql::execution::Database database(path, 2);
        require(database.execute("SELECT * FROM student;")["results"][0]["rows"].empty(), "deletion survives reopen");
    }
    const auto atomicPath = directory / "atomic.pages";
    {
        std::string fault;
        minisql::execution::Database database(atomicPath, 1, [&](std::string_view point) {
            if (point == fault) throw minisql::MiniSqlError(minisql::ErrorCode::Execution, "Injected commit failure");
        });
        fault = "prepared";
        require(database.execute("CREATE TABLE candidate(id INT);")["success"] == false, "failed create reports failure");
        require(database.catalog()["tables"].empty(), "failed create restores memory catalog");
        fault.clear();
        require(database.execute("CREATE TABLE candidate(id INT); INSERT INTO candidate VALUES(1);")["success"] == true,
                "rolled back table name and identifier reusable");
        for (const auto* sql : {"INSERT INTO candidate VALUES(2);", "INSERT INTO candidate VALUES(2),(3);", "UPDATE candidate SET id=3;", "DELETE FROM candidate;"}) {
            fault = "prepared";
            require(database.execute(sql)["success"] == false, "prepublication write fails");
            require(database.execute("SELECT * FROM candidate;")["results"][0]["rows"] == json::array({json::array({1})}),
                    "prepublication write restores rows and buffer");
        }
        fault = "published";
        const auto failed = database.execute("INSERT INTO candidate VALUES(2),(3);");
        require(failed["success"] == false && failed["commitState"] == "unknown", "published failure is indeterminate");
        require(database.execute("SELECT * FROM candidate;")["success"] == false, "indeterminate database rejects reads");
        bool rejected = false;
        try { (void)database.catalog(); } catch (const minisql::MiniSqlError&) { rejected = true; }
        require(rejected, "indeterminate database rejects catalog");
        rejected = false;
        try { (void)database.compile("SELECT * FROM candidate;"); } catch (const minisql::MiniSqlError&) { rejected = true; }
        require(rejected, "indeterminate database rejects compile");
    }
    {
        minisql::execution::Database database(atomicPath, 1);
        require(database.execute("SELECT * FROM candidate;")["results"][0]["rows"].size() == 3,
                "published multirow insert recovered completely on reopen");
    }
    const auto createPath = directory / "create-recovery.pages";
    {
        minisql::execution::Database database(createPath, 1, [](std::string_view point) {
            if (point == "published") throw minisql::MiniSqlError(minisql::ErrorCode::Execution, "Injected create failure");
        });
        require(database.execute("CREATE TABLE recovered(id INT PRIMARY KEY);")["commitState"] == "unknown",
                "published create reports unknown");
    }
    {
        minisql::execution::Database database(createPath, 1);
        require(database.catalog()["tables"].size() == 1, "published catalog recovered");
        require(database.execute("INSERT INTO recovered VALUES(1);")["success"] == true, "recovered schema usable");
        require(database.execute("INSERT INTO recovered VALUES(1);")["success"] == false, "recovered constraint enforced");
    }
    const auto sessionPath = directory / "session.pages";
    {
        minisql::execution::Database database(sessionPath, 1);
        require(database.execute("CREATE TABLE t(id INT PRIMARY KEY,n INT);")["success"] == true, "session fixture");
        require(database.execute("BEGIN;")["transactionState"] == "ACTIVE", "session begins");
        require(database.execute("INSERT INTO t VALUES(1,1),(2,2);")["results"][0]["commitState"] == "pending", "writes remain pending across calls");
        require(database.execute("SELECT * FROM t;")["results"][0]["rows"].size() == 2, "read own transaction writes");
        require(database.execute("INSERT INTO t VALUES(1,3);")["transactionState"] == "ABORTED", "constraint failure aborts session transaction");
        for (const auto* sql : {"SELECT * FROM t;", "INSERT INTO t VALUES(3,3);", "COMMIT;", "BEGIN;"})
            require(database.execute(sql)["error"]["code"] == 6001, "aborted session rejects nonrollback statement");
        require(database.execute("ROLLBACK;")["transactionState"] == "IDLE", "rollback acknowledges aborted transaction");
        require(database.execute("SELECT * FROM t;")["results"][0]["rows"].empty(), "prior pending statements rolled back");
        require(database.execute("BEGIN; CREATE TABLE tentative(id INT);")["success"] == true, "transactional catalog visible");
        require(database.compile("SELECT * FROM tentative;")["success"] == true, "compile sees own pending catalog");
        require(database.execute("ROLLBACK;")["success"] == true, "rollback pending catalog");
        require(database.catalog()["tables"].size() == 1, "pending catalog removed");
        require(database.execute("BEGIN; INSERT INTO t VALUES(4,4);")["success"] == true, "second transaction starts");
        require(database.execute("COMMIT;")["transactionState"] == "IDLE", "commit across calls");
        require(database.execute("COMMIT;")["error"]["code"] == 6001, "duplicate commit rejected");
        require(database.execute("BEGIN; INSERT INTO t VALUES(5,5);")["success"] == true, "pending transaction before disconnect");
    }
    {
        minisql::execution::Database database(sessionPath, 1);
        require(database.execute("SELECT * FROM t;")["results"][0]["rows"] == json::array({json::array({4,4})}), "disconnect drops uncommitted changes");
    }
    for (const auto& point : {"prepared", "published"}) {
        const auto faultPath = directory / (std::string("transaction-") + point + ".pages");
        {
            std::string fault;
            minisql::execution::Database database(faultPath, 1, [&](std::string_view event) {
                if (event == fault) throw std::runtime_error("Injected native transaction failure");
            });
            require(database.execute("CREATE TABLE t(id INT);")["success"] == true, "fault fixture");
            require(database.execute("BEGIN; INSERT INTO t VALUES(1); INSERT INTO t VALUES(2);")["success"] == true, "fault transaction pending");
            fault = point;
            const auto response = database.execute("COMMIT;");
            require(response["success"] == false, "commit exception converted to error");
            require(response["transactionState"] == (fault == "prepared" ? "ABORTED" : "UNKNOWN"), "commit point selects aborted or unknown state");
            if (fault == "prepared") {
                require(database.execute("ROLLBACK;")["success"] == true, "prepublication commit failure can be acknowledged");
                require(database.execute("SELECT * FROM t;")["results"][0]["rows"].empty(), "prepublication commit failure rolls back all statements");
            }
        }
        {
            minisql::execution::Database database(faultPath, 1);
            require(database.execute("SELECT * FROM t;")["results"][0]["rows"].size() == (std::string(point) == "published" ? 2 : 0), "transaction commit recovery is complete");
        }
    }
    std::cout << checks << " database execution checks passed\nEvidence: " << directory.string() << '\n';
}
