#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include "minisql/common/error.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace minisql::security {

// X25：权限检查消费的是绑定结果，而不是 SQL 文本。security 层因此不再依赖
// lexer/parser，也不再对 SQL 做任何字符串或词法扫描。
enum class AccessAction {
    Connect,      // 建立/关闭会话
    Compile,      // 只编译不执行（compile / diagnostics）
    Read,         // 读取元数据（catalog / statistics / buffer / indexinspect）
    Select,
    Insert,
    Update,
    Delete,
    Create,
    Drop,
    Transaction,  // BEGIN / COMMIT / ROLLBACK / SAVEPOINT
    Checkpoint    // CHECKPOINT / snapshot / restore
};

// 权限名（access catalog 中的字符串）与动作的唯一映射。
std::string permissionName(AccessAction action);

// 绑定器产出的单个受权对象：对象名已由 Catalog 规范化为物理名，动作是该对象
// 在本语句中实际承担的动作，而不是整条语句的动作。
// 例如 `DELETE FROM a WHERE id IN (SELECT id FROM b)` 产出
// {a, Delete} 与 {b, Select}，b 不再被保守地要求 DELETE 权限。
struct AccessObjectRef {
    std::string object;
    AccessAction action = AccessAction::Select;
    friend bool operator==(const AccessObjectRef& left, const AccessObjectRef& right) {
        return left.object == right.object && left.action == right.action;
    }
};

// 绑定结果到权限层的完整契约。`bound == false` 表示绑定器无法为这条语句给出
// 完整对象集合（语法不受支持、解析失败、引用了不存在的表等）；此时
// authorize 一律拒绝（fail-closed），绝不退回到文本扫描。
struct AccessRequest {
    AccessAction statementAction = AccessAction::Compile;
    std::vector<AccessObjectRef> objects{};
    bool bound = false;
    std::string diagnostic{};  // 未绑定时的原因，仅用于错误信息与审计。
    // 未绑定时按第十九章 HTTP 契约回报的真实 SQL 错误码（Ok 表示无可用原因，
    // 例如序列化计划文档形状不合法）。安全层仍然一律拒绝执行，只是不再把
    // 「SQL 检查失败」伪装成权限错误 403。
    ErrorCode failureCode = ErrorCode::Ok;
    SourceLocation failureLocation{};
};

class AccessCatalog {
// AccessCatalog 是权限目录：保存用户、口令与授权规则，并提供认证与鉴权。
public:
    // 从数据库同目录的 access.catalog.pages 读取权限目录；文件不存在时保持兼容的未启用状态。
    // 从数据库同目录的 access.catalog.pages 读取权限目录；文件不存在时保持兼容的未启用状态。
    static AccessCatalog load(const std::filesystem::path& databasePath);
    // （承接上一行）这个静态方法负责把权限目录文件读进来并解析成对象。
    // 从 PersistentCatalog 系统表恢复已校验的权限目录快照。
    // 从 PersistentCatalog 系统表恢复已校验的权限目录快照。
    static AccessCatalog fromDocument(nlohmann::json document, std::uint32_t permissionVersion);
    // 这个静态方法负责从系统表里恢复一份已经校验过的权限目录快照。

    bool enabled() const noexcept { return enabled_; }
    // 是否启用了权限控制；未启用时所有鉴权请求一律放行。
    std::uint32_t permissionVersion() const noexcept { return permissionVersion_; }
    // 返回权限目录版本号，用于判断是否需要重新加载。
    const nlohmann::json& document() const noexcept { return catalog_; }
    // 返回原始 JSON 文档，供观测与调试使用。
    bool verify(const std::string& user, const std::string& password) const;
    // 认证：校验用户名与口令是否正确。

    // operation 是入口动作（execute/compile/diagnostics/catalog/statistics/
    // indexinspect/close/snapshot/restore），request 是绑定结果。两者共同决定
    // 需要的权限：入口动作只能收紧（例如 compile 把 select 降级为 compile），
    // 对象集合完全来自绑定结果。
    void authorize(const std::string& user, const std::string& operation, const AccessRequest& request,
                   const std::string& table = {}, const std::string& index = {}) const;

private:
// 私有状态。
    void requireAll(const std::string& user, const std::string& permission,
                    const std::vector<std::string>& objects) const;

    bool enabled_ = false;
    // 是否启用权限控制；默认关闭以保持与旧行为兼容。
    std::uint32_t permissionVersion_ = 0;
    // 权限目录版本号。
    nlohmann::json catalog_;
    // 权限目录的完整文档。
};

} // namespace minisql::security
