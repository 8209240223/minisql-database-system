#include "minisql/execution/database.hpp"
#include "minisql/common/wire_json.hpp"
#include "minisql/security/access_catalog.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
using json = nlohmann::json;
// 简写 JSON 命名空间。
// 从 SQL 文件读取源码：显式失败优于静默空输入，并去掉 UTF-8 BOM。
// （承接上一行）"显式失败"指打不开文件就直接报错，而不是当成空脚本继续跑。
std::string readSqlFile(const std::filesystem::path& path) {
// 读取 SQL 文件内容。
    std::ifstream stream(path, std::ios::binary);
    // 以二进制方式打开，避免平台做换行转换。
    if (!stream) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot open SQL file: " + path.string());
    // 打不开就按参数错误抛出。
    std::string source{std::istreambuf_iterator<char>(stream), {}};
    // 一次性把文件内容读进字符串。
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        // 检查 UTF-8 BOM（EF BB BF）的前两个字节。
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF)
        // 检查 BOM 的第三个字节。
        source.erase(0, 3);
        // 去掉 BOM，否则第一个 token 会带上看不见的字符导致解析失败。
    return source;
    // 返回纯文本源码。
}
// 读取函数结束。
void authorizeRequest(minisql::execution::Database& database, const minisql::security::AccessCatalog& access, const json& request,
// 对一条会话请求做认证与鉴权：先从请求里取用户名口令，再判断有没有权限。
                      const std::string& operation, const std::string& sql = {},
                      // operation 是操作名；sql 可选，用于解析出实际访问的对象。
                      const std::string& table = {}, const std::string& index = {}) {
                      // table / index 用于 indexInspect 这类针对具体对象的操作。
    if (!access.enabled()) return;
    // 权限控制没启用时一律放行，保持与旧行为兼容。
    if (!request.contains("user") || !request["user"].is_string() ||
        // 请求里必须有字符串形式的 user 字段。
        !request.contains("password") || !request["password"].is_string() ||
        // 也必须有字符串形式的 password 字段。
        request["user"].get_ref<const std::string&>().empty() || request["user"].get_ref<const std::string&>().size() > 64 ||
        // 用户名不能为空，也不能超过 64 字符（防止异常长输入）。
        request["password"].get_ref<const std::string&>().size() > 4096)
        // 口令长度上限 4096，避免超长输入拖垮校验。
        throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
        // 任何一项不合规都按权限错误拒绝；这里刻意不给出细节，避免泄露可利用信息。
    const auto& user = request["user"].get_ref<const std::string&>();
    // 取出用户名。
    const auto& password = request["password"].get_ref<const std::string&>();
    // 取出口令。
    if (!access.verify(user, password)) throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
    // 认证失败同样统一报"权限不足"。
    const auto resolvedObjects = database.resolveAccessObjects(sql);
    // 用当前目录把 SQL 里实际访问的基础表解析成规范对象名，作为鉴权依据。
    access.authorize(user, operation, sql, table, index, resolvedObjects);
    // 执行鉴权：不通过时由 authorize 内部抛出权限错误。
}
// 请求鉴权函数结束。

void authorizeDirect(minisql::execution::Database& database, const minisql::security::AccessCatalog& access, const std::string& operation,
// 命令行直连模式下的鉴权：凭据来自环境变量而不是请求体。
                     const std::string& sql = {}) {
                     // sql 可选，用来解析出实际访问的对象。
    const auto* bypass = std::getenv("MINISQL_AUTH_BYPASS");
    // 读取绕过开关。
    if (!access.enabled() || (bypass && std::string(bypass) == "1")) return;
    // 权限未启用，或显式设置了绕过开关（常见于本地测试）时直接放行。
    const auto* configuredUser = std::getenv("MINISQL_USER");
    // 从环境变量取用户名。
    const auto* configuredPassword = std::getenv("MINISQL_PASSWORD");
    // 从环境变量取口令。
    const std::string user = configuredUser ? configuredUser : "";
    // 环境变量不存在时按空串处理，交给认证逻辑拒绝。
    const std::string password = configuredPassword ? configuredPassword : "";
    // 同上。
    if (!access.verify(user, password)) throw minisql::MiniSqlError(minisql::ErrorCode::Permission, "Permission denied");
    // 认证失败按权限错误上报。
    const auto resolvedObjects = database.resolveAccessObjects(sql);
    // 解析实际访问对象。
    access.authorize(user, operation, sql, {}, {}, resolvedObjects);
    // 直连模式没有表/索引参数，所以中间两个参数留空。
}
// 直连鉴权函数结束。

minisql::security::AccessCatalog reconcileAccessCatalog(minisql::execution::Database& database,
// 对齐两份权限目录：磁盘旁路文件里的（外部）与系统表里的（已持久化）。
                                                        const std::filesystem::path& databasePath) {
                                                        // databasePath 用于定位同目录下的权限目录文件。
    const auto external = minisql::security::AccessCatalog::load(databasePath);
    // 读取磁盘上的权限目录（文件不存在时是"未启用"状态）。
    const auto stored = database.accessCatalogRecord();
    // 读取已经持久化在系统表里的权限目录记录。
    const bool sameVersionConflict = external.enabled() && stored &&
        // 冲突场景之一：两边都启用，并且版本号相同。
        external.permissionVersion() == stored->permissionVersion && external.document().dump() != stored->payload;
        // 但内容却不一样，说明有人手改了文件，属于不可自动裁决的冲突。
    const bool externalIsNewer = external.enabled() && (!stored || external.permissionVersion() > stored->permissionVersion);
    // 外部版本更新的判断：系统表里没有，或者外部版本号更大。
    if (sameVersionConflict)
    // 同版本不同内容：直接拒绝启动，避免两份权限定义同时生效。
        throw minisql::MiniSqlError(minisql::ErrorCode::Catalog, "Access catalog version conflict");
        // 按目录层错误上报，提示运维人工处理。
    if (externalIsNewer) {
    // 外部版本更新，需要把它固化到系统表。
        if (std::string(database.transactionState()) == "IDLE") {
        // 只有当数据库处于空闲状态时才写系统表。
            database.synchronizeAccessCatalog(external.document(), external.permissionVersion());
            // 把新版本同步进系统表。
        } else {
        // 有活动事务时不能插入这次写入。
            // 活动事务不被目录同步写入打断；本次请求仍使用已校验的新页，事务结束后再固化到系统表。
            // 解释：本次请求照常使用刚校验过的新权限，等事务结束后再落到系统表，
            // 这样既不打断事务，也不会用旧权限放行。
            return external;
            // 直接返回外部目录供本次使用。
        }
    }
    const auto persisted = database.accessCatalogRecord();
    // 重新读一次系统表记录（上面可能刚写过）。
    if (persisted && (!external.enabled() || persisted->permissionVersion >= external.permissionVersion()))
    // 系统表里有记录，并且它不比外部旧。
        return minisql::security::AccessCatalog::fromDocument(
            // 用系统表里的快照构造权限目录。
            nlohmann::json::parse(persisted->payload), persisted->permissionVersion);
            // 把存的 JSON 文本解析回来，并带上版本号。
    return external;
    // 其余情况以外部目录为准。
}
// 目录对齐函数结束。

int session(minisql::execution::Database& database, minisql::security::AccessCatalog access,
// 会话主循环：按行读写 JSON 请求与响应，一次进程可以服务多条请求。
            const std::filesystem::path& databasePath) {
            // databasePath 用于每次请求前重新对齐权限目录。
    const auto emit = [](json value) {
    // 局部工具：把一条响应写到标准输出。
        value["integerEncoding"] = "safe-number-or-decimal-string";
        // 统一标注大整数编码规则，前端据此决定怎么解析大整数。
        std::cout << minisql::wireJson(std::move(value)).dump() << '\n' << std::flush;
        // 输出一行 JSON 并立即刷新，保证交互式调用方立刻收到响应。
    };
    // emit 定义结束。
    emit({{"type", "ready"}, {"protocolVersion", 1}, {"transactionState", database.transactionState()}});
    // 先发一条 ready 握手消息，告诉调用方协议版本与当前事务状态。
    constexpr std::size_t maxFrameBytes = 8 * 1024 * 1024;
    // 单帧上限 8 MiB：防止一次请求把内存吃满。
    for (;;) {
    // 一条请求一帧，循环处理直到退出。
        std::string frame;
        // 当前帧的原始文本。
        bool overflow = false, complete = false;
        // overflow 记录是否超限；complete 记录是否读到了行尾。
        char ch;
        // 逐字符读取的缓冲。
        while (std::cin.get(ch)) {
        // 一直读到本帧结束或输入结束。
            if (ch == '\n') { complete = true;break; }
            // 换行表示这一帧结束。
            if (frame.size() < maxFrameBytes) frame.push_back(ch);
            // 还没超限就继续累积。
            else overflow = true;
            // 超限后只置标记、不再增长内存。
        }
        if (!complete && frame.empty() && !overflow) return 0;
        // 输入正常结束且没有残留内容，说明调用方主动关闭，正常退出。
        json result, id = nullptr;
        // 本条的响应对象，以及从请求里回显的 id。
        bool close = false;
        // 是否收到关闭请求。
        bool streamRequested = false;
        // 请求的是否是流式执行。
        bool streamed = false;
        // 是否已经真正走过流式路径（用于决定错误如何回报）。
        try {
        // 单条请求的全部处理包在 try 里，任何错误都转成一条错误响应而不是杀掉会话。
            if (!complete) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unterminated session frame");
            // 没读到换行就结束了，说明帧被截断。
            if (overflow) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Session frame exceeds 8 MiB");
            // 超过 8 MiB 上限。
            const auto request = json::parse(frame, [](int depth, json::parse_event_t, json&) {
            // 解析这一帧，并挂一个解析回调用来限制嵌套深度。
                if (depth > 16) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Session frame nesting limit exceeded");
                // 深度超过 16 层就拒绝，避免深层嵌套造成栈或内存问题。
                return true;
                // 返回 true 表示正常保留这个事件。
            });
            // 解析结束，得到请求对象。
            if (!request.is_object() || !request.contains("id") || !request["id"].is_string() ||
            // 请求必须是对象，且带字符串类型的 id 字段。
                request["id"].get_ref<const std::string&>().empty() || request["id"].get_ref<const std::string&>().size() > 128 ||
                // id 不能为空，也不能超过 128 字符。
                !request.contains("operation") || !request["operation"].is_string())
                // 还必须有字符串类型的 operation 字段。
                throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected id and operation strings");
                // 不满足就按参数错误拒绝。
            id = request["id"];
            // 记下 id，稍后原样回显，便于调用方把响应与请求对应起来。
            for (const auto& [name, value] : request.items()) {
            // 遍历请求的全部字段，做字段白名单校验。
                (void)value;
                // 只看字段名，值不使用，显式标记忽略以免告警。
                    if (name != "id" && name != "operation" && name != "sql" && name != "sessionId" && name != "cancelFile" && name != "table" && name != "index" && name != "target" && name != "user" && name != "password")
                    // 只允许这些字段名。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown session request field");
                    // 出现未知字段就拒绝，避免拼错字段名却被静默忽略。
            }
            // 白名单校验结束。
            const auto operation = request["operation"].get<std::string>();
            // 取出操作名。
            const auto refreshedAccess = reconcileAccessCatalog(database, databasePath);
            // 每条请求前重新对齐权限目录，这样改权限后无需重启即可生效。
            if (refreshedAccess.enabled() != access.enabled() ||
            // 启用状态或版本号发生变化时，
                refreshedAccess.permissionVersion() != access.permissionVersion())
                // 判断依据就是这两项。
                access = refreshedAccess;
                // 换用最新的权限目录。
            if (request.contains("sessionId")) {
            // 请求带了会话标识，说明要设置会话上下文。
                if (!request["sessionId"].is_string() || request["sessionId"].get_ref<const std::string&>().empty() ||
                // 会话号必须是字符串且非空。
                    request["sessionId"].get_ref<const std::string&>().size() > 128)
                    // 长度上限 128。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected session id string");
                    // 不合规就拒绝。
                if (request.contains("cancelFile") && !request["cancelFile"].is_string())
                // cancelFile 若给出，必须是字符串。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected cancellation file string");
                    // 否则拒绝。
                database.setSessionContext(request["sessionId"].get<std::string>(),
                    // 把会话号交给数据库。
                    request.contains("cancelFile") ? std::filesystem::path(request["cancelFile"].get<std::string>()) : std::filesystem::path{});
                    // 取消标记文件给了就用它，没给就传空路径表示不启用取消。
            }
            // 会话上下文处理结束。
            if (operation == "execute" || operation == "compile") {
            // 执行与编译共用同一段取 SQL 的逻辑。
                if (!request.contains("sql") || !request["sql"].is_string())
                // 两者都要求有字符串形式的 sql 字段。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                    // 缺失就拒绝。
                const auto source = request["sql"].get<std::string>();
                // 取出 SQL 文本。
                authorizeRequest(database, access, request, operation, source);
                // 先鉴权，再执行；未授权时不会走到下一步。
                result = operation == "execute" ? database.execute(source) : database.compile(source);
                // execute 真正执行，compile 只编译并返回计划。
            } else if (operation == "executeStream") {
            // 流式执行：边算边推，适合大结果集。
                if (!request.contains("sql") || !request["sql"].is_string())
                // 同样要求 sql 字段。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                    // 缺失就拒绝。
                streamRequested = true;
                // 标记这是流式请求，后面出错时要按流式协议回报。
                const auto source = request["sql"].get<std::string>();
                // 取出 SQL 文本。
                authorizeRequest(database, access, request, "execute", source);
                // 鉴权时把操作名统一成 execute（流式只是传输形态不同）。
                const auto summary = database.executeStreaming(source,
                // 开始流式执行；下面两个回调分别处理元数据与每一行。
                    [&](const json& meta) {
                    // 元数据回调：只在开始时触发一次。
                        emit({{"id", id}, {"stream", true}, {"type", "meta"}, {"meta", meta}});
                        // 推送一条 meta 帧。
                    },
                    // 元数据回调结束。
                    [&](const json& row) {
                    // 行回调：每行触发一次。
                        emit({{"id", id}, {"stream", true}, {"type", "row"}, {"row", row}});
                        // 推送一条 row 帧。
                        return static_cast<bool>(std::cout);
                        // 返回输出流是否仍然可用；调用方写不进去时执行会提前停下。
                    });
                // 流式执行结束，summary 里是行数与资源用量。
                emit({{"id", id}, {"stream", true}, {"type", "complete"}, {"success", true}, {"rows", summary.value("rows", std::size_t{0})},
                    // 推送 complete 帧，表示本次流式结果已经发完。
                    {"resourceUsage", summary.value("resourceUsage", json::object())}});
                    // 附带资源用量，便于上层做预算与观测。
                streamed = true;
                // 标记流式路径已经真正走过。
                result = {{"success", true}};
                // 主响应只保留一个成功标记，具体内容已经在流里发过了。
            } else if (operation == "buffer") {
            // 缓冲池配置操作（如查看状态、扩容）。
                if (!request.contains("sql") || !request["sql"].is_string())
                // 该操作复用 sql 字段传递动作名。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected buffer action string");
                    // 缺失就拒绝。
                authorizeRequest(database, access, request, operation, request["sql"].get<std::string>());
                result = database.configureBuffer(request["sql"].get<std::string>());
                // 执行缓冲池配置调整。
            } else if (operation == "diagnostics") {
            // 诊断：解析 SQL 并返回词法、语法、语义层面的错误定位。
                if (!request.contains("sql") || !request["sql"].is_string())
                // 要求 sql 字段。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected SQL string");
                    // 缺失就拒绝。
                authorizeRequest(database, access, request, operation, request["sql"].get<std::string>());
                result = database.diagnostics(request["sql"].get<std::string>());
                // 生成诊断信息。
            } else if (operation == "statistics") { authorizeRequest(database, access, request, operation); result = database.statistics(); }
            // 统计信息：只读操作，无需 SQL 参数。
            else if (operation == "catalog") { authorizeRequest(database, access, request, operation); result = database.catalog(); }
            // 目录内容：同样只读。
            else if (operation == "indexInspect") {
            // 索引结构检查：针对某张表的某个索引。
                if (!request.contains("table") || !request["table"].is_string() ||
                // 需要字符串形式的 table 字段。
                    !request.contains("index") || !request["index"].is_string())
                    // 以及字符串形式的 index 字段。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected table and index strings");
                    // 任一缺失都拒绝。
                authorizeRequest(database, access, request, operation, {}, request["table"].get<std::string>(), request["index"].get<std::string>());
                // 鉴权时把表名与索引名一起交给权限层，便于按对象判断权限。
                result = database.indexInspect(request["table"].get<std::string>(), request["index"].get<std::string>());
                // 执行索引结构检查。
            } else if (operation == "snapshot") {
            // 快照：把当前数据库复制到目标目录。
                if (!request.contains("target") || !request["target"].is_string())
                // 需要字符串形式的 target 字段。
                    throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Expected snapshot target string");
                    // 缺失就拒绝。
                authorizeRequest(database, access, request, operation);
                result = database.createSnapshot(request["target"].get<std::string>());
                // 创建快照。
            } else if (operation == "close") {
            // 关闭会话。
                authorizeRequest(database, access, request, operation);
                close = true;
                // 标记处理完这条请求后退出循环。
                if (std::string(database.transactionState()) == "ACTIVE" || std::string(database.transactionState()) == "ABORTED")
                // 如果还有未结束（或已中止）的事务，
                    result = database.execute("ROLLBACK;");
                    // 先回滚再关闭，避免留下半途而废的事务状态。
                else result = {{"success", true}};
                // 否则直接返回成功。
            } else throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown session operation");
            // 未知操作一律拒绝。
            if (!result.contains("success")) result["success"] = true;
            // 统一补齐 success 字段，调用方不必为每个操作单独判断。
        } catch (const minisql::MiniSqlError& error) { result = error.toJson(); }
        // 已识别错误：整体替换成错误对象。
        catch (const json::exception&) {
        // JSON 解析或类型取值出错。
            result = minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Malformed session JSON").toJson();
            // 统一报"会话 JSON 格式错误"。
        } catch (const std::exception& error) {
        // 其它未预期异常。
            result = minisql::MiniSqlError(minisql::ErrorCode::Internal, std::string("Session operation failed: ") + error.what()).toJson();
            // 包装成内部错误，并带上原始描述便于排查。
        }
        // 单条请求的异常处理结束。
        if (streamRequested && !streamed) {
        // 请求本来是流式，但在真正开始推流之前就失败了。
            result["stream"] = true;
            // 仍然按流式协议回报。
            result["type"] = "error";
            // 让调用方知道这是流式请求的错误，而不是普通响应。
        }
        if (!streamed) {
        // 非流式请求要补齐两个公共字段。
            result["id"] = id;
            // 回显请求 id。
            result["transactionState"] = database.transactionState();
            // 告知当前事务状态，便于调用方决定下一步。
        }
        const bool failed = !result.value("success", false);
        // 判断本条是否失败。
        if (!streamed) emit(std::move(result));
        // 非流式时才写这一条响应（流式内容已经在过程中发完了）。
        if (!std::cout || !complete || close || std::string(database.transactionState()) == "UNKNOWN") return failed ? 1 : 0;
        // 退出条件：输出流损坏、帧不完整、收到 close，或事务状态已不可知。
    }
}
// 会话函数结束。
}

int main(int argc, char** argv) {
// 数据库进程入口：解析位置参数，打开数据库，按模式执行并输出 JSON。
    try {
    // 所有错误统一在这里转成 JSON 输出。
        // 位置参数：<database.pages> <mode>；可选 `--file/-f <query.sql>` 从文件读 SQL（默认标准输入）。
        // （承接上一行）mode 可取 execute、compile、diagnostics、statistics、catalog、session。
        std::filesystem::path sqlFile;
        // --file 指定的 SQL 文件；为空表示从标准输入读。
        std::vector<std::string> positional;
        // 收集所有位置参数。
        int databaseArgIndex = -1;
        // 记录第一个位置参数在 argv 里的下标，Windows 下需要用它取回原始 Unicode 路径。
        for (int index = 1; index < argc; ++index) {
        // 遍历命令行参数。
            const std::string argument = argv[index];
            // 当前参数。
            if (argument == "--file" || argument == "-f") {
            // 命中文件选项。
                if (index + 1 >= argc) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "--file requires a path");
                // 后面没跟路径就报参数错误。
                sqlFile = argv[++index];
                // 取下一个参数作为路径并跳过它。
                continue;
                // 继续处理后续参数。
            }
            if (databaseArgIndex < 0) databaseArgIndex = index;
            // 第一个非选项参数就是数据库文件路径参数。
            positional.push_back(argument);
            // 记入位置参数列表。
        }
        if (positional.size() != 2) throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument,
        // 位置参数必须恰好两个：数据库路径与模式。
            "Usage: minisql_database <database.pages> <execute|compile|diagnostics|statistics|catalog|session> [--file <query.sql>]");
            // 数量不对时打印用法提示。
        const std::string mode = positional[1];
        // 取出模式。
        if (mode != "execute" && mode != "compile" && mode != "diagnostics" && mode != "statistics" && mode != "catalog" && mode != "session")
        // 模式必须在白名单内。
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Unknown database command");
            // 未知模式直接拒绝，避免误以为执行成功。
        std::filesystem::path path;
        // 数据库文件路径。
#ifdef _WIN32
        int wideCount = 0;
        // 接收宽字符参数个数。
        auto wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
        // 按 Windows 规则重新拆分命令行，得到真正的宽字符参数。
        if (!wideArgs || wideCount != argc) {
        // 拆分失败，或拆分结果与窄字符参数个数不一致。
            if (wideArgs) LocalFree(wideArgs);
            // 先释放已经分配的宽字符数组。
            throw minisql::MiniSqlError(minisql::ErrorCode::InvalidArgument, "Cannot decode command line");
            // 按参数错误上报：无法可靠解码命令行时宁可不启动。
        }
        path = wideArgs[databaseArgIndex];
        // 用宽字符版本的同一位置参数作为数据库路径，从而正确支持中文等非 ASCII 路径。
        LocalFree(wideArgs);
        // 释放宽字符数组。
#else
        path = argv[1];
        // 直接用第一个位置参数即可。
#endif
        minisql::execution::Database database(path);
        // 打开（或创建）数据库。
        auto access = reconcileAccessCatalog(database, path);
        // 对齐权限目录，确定本次进程使用哪一份权限定义。
        if (mode == "session") return session(database, std::move(access), path);
        // session 模式交给会话循环处理，它自己负责输出协议。
        nlohmann::json result;
        // 单次调用模式的统一结果对象。
        if (mode == "catalog") { authorizeDirect(database, access, mode); result = database.catalog(); }
        // catalog 模式只需要导出目录，不需要读 SQL。
        else {
        // 其余模式都要 SQL 文本。
            const std::string source = sqlFile.empty() ? std::string{std::istreambuf_iterator<char>(std::cin), {}} : readSqlFile(sqlFile);
            // 没给文件就读标准输入，否则读文件。
            authorizeDirect(database, access, mode, source);
            // 带上 SQL 做鉴权，这样权限层能解析出实际访问的对象。
            result = mode == "execute" ? database.executeScript(source) : mode == "diagnostics" ? database.diagnostics(source) : mode == "statistics" ? database.statistics() : database.compile(source);
            // 依次分派：execute 执行脚本、diagnostics 出诊断、statistics 出统计、compile 只编译。
        }
        result["integerEncoding"] = "safe-number-or-decimal-string";
        // 标注大整数编码规则。
        std::cout << minisql::wireJson(result).dump() << '\n';
        // 输出结果 JSON。
        return result.value("success", true) ? 0 : 1;
        // 成功返回 0，失败返回 1；默认视为成功是为兼容没有 success 字段的老结果。
    } catch (const minisql::MiniSqlError& error) {
    // 已识别错误。
        std::cout << error.toJson().dump() << '\n';
        // 输出错误 JSON。
        return 1;
        // 失败退出码。
    } catch (const std::exception& error) {
    // 未预期异常。
        std::cout << minisql::MiniSqlError(minisql::ErrorCode::Internal, std::string("Database operation failed: ") + error.what()).toJson().dump() << '\n';
        // 包装成内部错误后输出。
        return 1;
        // 失败退出码。
    }
}
// 入口函数结束。
