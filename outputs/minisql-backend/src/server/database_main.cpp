#include "minisql/execution/database.hpp"
#include "minisql/common/wire_json.hpp"
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using json = nlohmann::json;
int session(minisql::execution::Database& database) {
    const auto emit = [](json value) {
        value["integerEncoding"] = "safe-number-or-decimal-string";
        std::cout << minisql::wireJson(std::move(value)).dump() << '\n' << std::flush;
    };
    // 流式 NDJSON 帧：与 emit 相同，但不附加 integerEncoding（逐行协议由 HTTP 层声明编码）。
    const auto emitStream = [](json value) {
        std::cout << minisql::wireJson(std::move(value)).dump() << '\n' << std::flush;
    };
    // 读取跨进程背压控制帧：{"ack": id} 表示下游已消费上一行并允许继续；{"cancel": id} 取消。
    constexpr std::size_t maxControlBytes = 1024;
    const auto readStreamControl = [](const json& id) {
        std::string line;
        bool overflow = false, complete = false;
        char ch;
        while (std::cin.get(ch)) {
            if (ch == '\n') { complete = true; break; }
            if (line.size() < maxControlBytes) line.push_back(ch);
            else overflow = true;
        }
        if (!complete) throw minisql::MiniSqlError(minisql::ErrorCode::Cancelled, "Stream peer closed during transfer");
        if (overflow) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Stream control frame exceeds 1 KiB");
        try {
            const auto control = json::parse(line, [](int depth, json::parse_event_t, json&) {
                if (depth > 4) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Stream control nesting limit exceeded");
                return true;
            });
            if (control.value("cancel", std::string{}) == id.get<std::string>())
                throw minisql::MiniSqlError(minisql::ErrorCode::Cancelled, "Query cancelled by client");
            if (control.value("ack", std::string{}) == id.get<std::string>()) return;
        } catch (const minisql::MiniSqlError&) { throw; }
        catch (...) {}
        throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected stream acknowledgement");
    };
    emit({{"type", "ready"}, {"protocolVersion", 1}, {"transactionState", database.transactionState()}});
    constexpr std::size_t maxFrameBytes = 8 * 1024 * 1024;
    for (;;) {
        std::string frame;
        bool overflow = false, complete = false;
        char ch;
        while (std::cin.get(ch)) {
            if (ch == '\n') { complete = true;break; }
            if (frame.size() < maxFrameBytes) frame.push_back(ch);
            else overflow = true;
        }
        if (!complete && frame.empty() && !overflow) return 0;
        json result, id = nullptr;
        bool close = false;
        try {
            if (!complete) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unterminated session frame");
            if (overflow) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Session frame exceeds 8 MiB");
            const auto request = json::parse(frame, [](int depth, json::parse_event_t, json&) {
                if (depth > 16) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Session frame nesting limit exceeded");
                return true;
            });
            if (!request.is_object() || !request.contains("id") || !request["id"].is_string() ||
                request["id"].get_ref<const std::string&>().empty() || request["id"].get_ref<const std::string&>().size() > 128 ||
                !request.contains("operation") || !request["operation"].is_string())
                throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected id and operation strings");
            id = request["id"];
            for (const auto& [name, value] : request.items()) {
                (void)value;
                if (name != "id" && name != "operation" && name != "sql" && name != "sessionId" && name != "cancelFile")
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown session request field");
            }
            const auto operation = request["operation"].get<std::string>();
            if (request.contains("sessionId")) {
                if (!request["sessionId"].is_string() || request["sessionId"].get_ref<const std::string&>().empty() ||
                    request["sessionId"].get_ref<const std::string&>().size() > 128)
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected session id string");
                if (request.contains("cancelFile") && !request["cancelFile"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected cancellation file string");
                database.setSessionContext(request["sessionId"].get<std::string>(),
                    request.contains("cancelFile") ? std::filesystem::path(request["cancelFile"].get<std::string>()) : std::filesystem::path{});
            }
            if (operation == "execute" || operation == "compile") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                const auto source = request["sql"].get<std::string>();
                result = operation == "execute" ? database.execute(source) : database.compile(source);
            } else if (operation == "buffer") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected buffer action string");
                result = database.configureBuffer(request["sql"].get<std::string>());
            } else if (operation == "diagnostics") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                result = database.diagnostics(request["sql"].get<std::string>());
            } else if (operation == "statistics") result = database.statistics();
            else if (operation == "catalog") result = database.catalog();
            else if (operation == "indexInspect") {
                if (!request.contains("table") || !request["table"].is_string() ||
                    !request.contains("index") || !request["index"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected table and index strings");
                result = database.indexInspect(request["table"].get<std::string>(), request["index"].get<std::string>());
            } else if (operation == "stream") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                const auto source = request["sql"].get<std::string>();
                try {
                    json columns = json::array(), columnTypes = json::array();
                    std::size_t streamedRows = 0;
                    const auto summary = database.streamQuery(source,
                        [&](json&& meta) {
                            columns = meta.at("columns");
                            columnTypes = meta.at("columnTypes");
                            emitStream({{"type", "meta"}, {"id", id}, {"success", true}, {"columns", columns}, {"columnTypes", columnTypes}});
                        },
                        [&](minisql::execution::Row&& row) {
                            emitStream({{"type", "row"}, {"id", id}, {"index", streamedRows}, {"values", std::move(row)}});
                            ++streamedRows;
                        },
                        [&](std::size_t) { readStreamControl(id); });
                    emitStream({{"type", "complete"}, {"id", id}, {"success", true}, {"rowCount", summary.at("rowCount")},
                                {"columns", columns}, {"columnTypes", columnTypes},
                                {"transactionState", database.transactionState()}});
                } catch (const minisql::MiniSqlError& error) {
                    emitStream({{"type", "error"}, {"id", id}, {"success", false}, {"error", error.toJson()},
                                {"transactionState", database.transactionState()}});
                    if (!std::cout) return 1;
                } catch (const std::exception& error) {
                    emitStream({{"type", "error"}, {"id", id}, {"success", false},
                                {"error", minisql::MiniSqlError(minisql::ErrorCode::Internal, std::string("Stream operation failed: ") + error.what()).toJson()},
                                {"transactionState", database.transactionState()}});
                    if (!std::cout) return 1;
                }
                if (!std::cout) return 1;
                continue;
            } else if (operation == "close") {
                close = true;
                if (std::string(database.transactionState()) == "ACTIVE" || std::string(database.transactionState()) == "ABORTED")
                    result = database.execute("ROLLBACK;");
                else result = {{"success", true}};
            } else throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown session operation");
            if (!result.contains("success")) result["success"] = true;
        } catch (const minisql::MiniSqlError& error) { result = error.toJson(); }
        catch (const json::exception&) {
            result = minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Malformed session JSON").toJson();
        } catch (const std::exception& error) {
            result = minisql::MiniSqlError(minisql::ErrorCode::Internal, std::string("Session operation failed: ") + error.what()).toJson();
        }
        result["id"] = id;
        result["transactionState"] = database.transactionState();
        const bool failed = !result.value("success", false);
        emit(std::move(result));
        if (!std::cout || !complete || close || std::string(database.transactionState()) == "UNKNOWN") return failed ? 1 : 0;
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Usage: minisql_database <database.pages> <execute|compile|diagnostics|statistics|catalog|session|snapshot>");
        const std::string mode = argv[2];
        if (mode != "execute" && mode != "compile" && mode != "diagnostics" && mode != "statistics" && mode != "catalog" && mode != "session" && mode != "snapshot")
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown database command");
        std::filesystem::path path;
#ifdef _WIN32
        int wideCount = 0;
        auto wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        if (!wideArgs || wideCount != argc) {
            if (wideArgs) LocalFree(wideArgs);
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot decode command line");
        }
        path = wideArgs[1];
        LocalFree(wideArgs);
#else
        path = argv[1];
#endif
        minisql::execution::Database database(path);
        if (mode == "session") return session(database);
        nlohmann::json result;
        if (mode == "snapshot") result = database.snapshotInfo();
        else if (mode == "catalog") result = database.catalog();
        else {
            const std::string source{std::istreambuf_iterator<char>(std::cin), {}};
            result = mode == "execute" ? database.executeScript(source) : mode == "diagnostics" ? database.diagnostics(source) : mode == "statistics" ? database.statistics() : database.compile(source);
        }
        result["integerEncoding"] = "safe-number-or-decimal-string";
        std::cout << minisql::wireJson(result).dump() << '\n';
        return result.value("success", true) ? 0 : 1;
    } catch (const minisql::MiniSqlError& error) {
        std::cout << error.toJson().dump() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cout << minisql::MiniSqlError(minisql::ErrorCode::Internal, std::string("Database operation failed: ") + error.what()).toJson().dump() << '\n';
        return 1;
    }
}
