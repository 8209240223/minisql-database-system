#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace minisql::security {

class AccessCatalog {
public:
    // 从数据库同目录的 access.catalog.pages 读取权限目录；文件不存在时保持兼容的未启用状态。
    static AccessCatalog load(const std::filesystem::path& databasePath);

    bool enabled() const noexcept { return enabled_; }
    std::uint32_t permissionVersion() const noexcept { return permissionVersion_; }
    bool verify(const std::string& user, const std::string& password) const;
    void authorize(const std::string& user, const std::string& operation, const std::string& sql,
                   const std::string& table = {}, const std::string& index = {}) const;

private:
    bool enabled_ = false;
    std::uint32_t permissionVersion_ = 0;
    nlohmann::json catalog_;
};

} // namespace minisql::security
