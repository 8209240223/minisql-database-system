#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>
#include <vector>

namespace minisql::security {

class AccessCatalog {
// AccessCatalog 是权限目录：保存用户、口令与授权规则，并提供认证与鉴权。
public:
    // 从数据库同目录的 access.catalog.pages 读取权限目录；文件不存在时保持兼容的未启用状态。
    static AccessCatalog load(const std::filesystem::path& databasePath);
    // （承接上一行）这个静态方法负责把权限目录文件读进来并解析成对象。
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
    void authorize(const std::string& user, const std::string& operation, const std::string& sql,
    // 鉴权：判断该用户是否有权执行这条语句，缺对象时给出可读的错误信息。
                   const std::string& table = {}, const std::string& index = {},
                   const std::vector<std::string>& resolvedObjects = {}) const;
                   // （承接上一行）resolvedObjects 是已经解析出真实名字的对象列表。

private:
// 私有状态。
    bool enabled_ = false;
    // 是否启用权限控制；默认关闭以保持与旧行为兼容。
    std::uint32_t permissionVersion_ = 0;
    // 权限目录版本号。
    nlohmann::json catalog_;
    // 权限目录的完整文档。
};

} // namespace minisql::security
