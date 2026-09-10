#include "minisql/execution/database.hpp"
#include "minisql/common/wire_json.hpp"
#include "minisql/security/access_catalog.hpp"
#include <cstdlib>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using json = nlohmann::json;
void authorizeRequest(minisql::execution::Database& database, const minisql::security::AccessCatalog& access, const json& request,
                      const std::string& operation, const std::string& sql = {},
                      const std::string& table = {}, const std::string& index = {}) {
    if (!access.enabled()) return;
    if (!request.contains("user") || !request["user"].is_string() ||
        !request.contains("password") || !request["password"].is_string() ||
        request["user"].get_ref<const std::string&>().empty() || request["user"].get_ref<const std::string&>().size() > 64 ||
        request["password"].get_ref<const std::string&>().size() > 4096)
        throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
    const auto& user = request["user"].get_ref<const std::string&>();
    const auto& password = request["password"].get_ref<const std::string&>();
    if (!access.verify(user, password)) throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
    const auto resolvedObjects = database.resolveAccessObjects(sql);
    access.authorize(user, operation, sql, table, index, resolvedObjects);
}

void authorizeDirect(minisql::execution::Database& database, const minisql::security::AccessCatalog& access, const std::string& operation,
                     const std::string& sql = {}) {
    const auto* bypass = std::getenv("MINISQL_AUTH_BYPASS");
    if (!access.enabled() || (bypass && std::string(bypass) == "1")) return;
    const auto* configuredUser = std::getenv("MINISQL_USER");
    const auto* configuredPassword = std::getenv("MINISQL_PASSWORD");
    const std::string user = configuredUser ? configuredUser : "";
    const std::string password = configuredPassword ? configuredPassword : "";
    if (!access.verify(user, password)) throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
    const auto resolvedObjects = database.resolveAccessObjects(sql);
    access.authorize(user, operation, sql, {}, {}, resolvedObjects);
}

minisql::security::AccessCatalog reconcileAccessCatalog(minisql::execution::Database& database,
                                                        const std::filesystem::path& databasePath) {
    const auto external = minisql::security::AccessCatalog::load(databasePath);
    const auto stored = database.accessCatalogRecord();
    const bool sameVersionConflict = external.enabled() && stored &&
        external.permissionVersion() == stored->permissionVersion && external.document().dump() != stored->payload;
    const bool externalIsNewer = external.enabled() && (!stored || external.permissionVersion() > stored->permissionVersion);
    if (sameVersionConflict)
        throw minisql::MiniSqlError(minisql::ErrorCode::Catalog, "Access catalog version conflict");
    if (externalIsNewer) {
        if (std::string(database.transactionState()) == "IDLE") {
            database.synchronizeAccessCatalog(external.document(), external.permissionVersion());
        } else {
            // 活动事务不被目录同步写入打断；本次请求仍使用已校验的新页，事务结束后再固化到系统表。
            return external;
        }
    }
    const auto persisted = database.accessCatalogRecord();
    if (persisted && (!external.enabled() || persisted->permissionVersion >= external.permissionVersion()))
        return minisql::security::AccessCatalog::fromDocument(
            nlohmann::json::parse(persisted->payload), persisted->permissionVersion);
    return external;
}

int session(minisql::execution::Database& database, minisql::security::AccessCatalog access,
            const std::filesystem::path& databasePath) {
    const auto emit = [](json value) {
        value["integerEncoding"] = "safe-number-or-decimal-string";
        std::cout << minisql::wireJson(std::move(value)).dump() << '\n' << std::flush;
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
                    if (name != "id" && name != "operation" && name != "sql" && name != "sessionId" && name != "cancelFile" && name != "table" && name != "index" && name != "target" && name != "user" && name != "password")
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown session request field");
            }
            const auto operation = request["operation"].get<std::string>();
            const auto refreshedAccess = reconcileAccessCatalog(database, databasePath);
            if (refreshedAccess.enabled() != access.enabled() ||
                refreshedAccess.permissionVersion() != access.permissionVersion())
                access = refreshedAccess;
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
                authorizeRequest(database, access, request, operation, source);
                result = operation == "execute" ? database.execute(source) : database.compile(source);
            } else if (operation == "buffer") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected buffer action string");
                authorizeRequest(database, access, request, operation, request["sql"].get<std::string>());
                result = database.configureBuffer(request["sql"].get<std::string>());
            } else if (operation == "diagnostics") {
                if (!request.contains("sql") || !request["sql"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                authorizeRequest(database, access, request, operation, request["sql"].get<std::string>());
                result = database.diagnostics(request["sql"].get<std::string>());
            } else if (operation == "statistics") { authorizeRequest(database, access, request, operation); result = database.statistics(); }
            else if (operation == "catalog") { authorizeRequest(database, access, request, operation); result = database.catalog(); }
            else if (operation == "indexInspect") {
                if (!request.contains("table") || !request["table"].is_string() ||
                    !request.contains("index") || !request["index"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected table and index strings");
                authorizeRequest(database, access, request, operation, {}, request["table"].get<std::string>(), request["index"].get<std::string>());
                result = database.indexInspect(request["table"].get<std::string>(), request["index"].get<std::string>());
            } else if (operation == "snapshot") {
                if (!request.contains("target") || !request["target"].is_string())
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected snapshot target string");
                authorizeRequest(database, access, request, operation);
                result = database.createSnapshot(request["target"].get<std::string>());
            } else if (operation == "close") {
                authorizeRequest(database, access, request, operation);
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
        if (argc != 3) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Usage: minisql_database <database.pages> <execute|compile|diagnostics|statistics|catalog|session>");
        const std::string mode = argv[2];
        if (mode != "execute" && mode != "compile" && mode != "diagnostics" && mode != "statistics" && mode != "catalog" && mode != "session")
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
        auto access = reconcileAccessCatalog(database, path);
        if (mode == "session") return session(database, std::move(access), path);
        nlohmann::json result;
        if (mode == "catalog") { authorizeDirect(database, access, mode); result = database.catalog(); }
        else {
            const std::string source{std::istreambuf_iterator<char>(std::cin), {}};
            authorizeDirect(database, access, mode, source);
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
