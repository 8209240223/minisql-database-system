#include "minisql/security/access_catalog.hpp"
#include "minisql/common/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace minisql::security {
namespace {

constexpr std::size_t pageSize = 4096;
constexpr std::size_t metaHeaderBytes = 72;
constexpr std::size_t dataHeaderBytes = 16;
constexpr std::size_t dataCapacity = pageSize - dataHeaderBytes;

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw MiniSqlError(ErrorCode::Storage, "Access catalog header is truncated");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint32_t fnv1a(const std::uint8_t* bytes, std::size_t length) {
    std::uint32_t hash = 0x811c9dc5U;
    for (std::size_t index = 0; index < length; ++index) {
        hash ^= bytes[index];
        hash *= 0x01000193U;
    }
    return hash;
}

std::uint32_t rotateRight(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
}

std::array<std::uint8_t, 32> sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const auto bitLength = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while (message.size() % 64U != 56U) message.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<std::uint8_t>(bitLength >> shift));

    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto offset = block + index * 4;
            words[index] = (static_cast<std::uint32_t>(message[offset]) << 24) |
                (static_cast<std::uint32_t>(message[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(message[offset + 2]) << 8) |
                static_cast<std::uint32_t>(message[offset + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto s0 = rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const auto s1 = rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto working = state;
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto choice = (working[4] & working[5]) ^ (~working[4] & working[6]);
            const auto majority = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
            const auto sum1 = rotateRight(working[4], 6) ^ rotateRight(working[4], 11) ^ rotateRight(working[4], 25);
            const auto sum0 = rotateRight(working[0], 2) ^ rotateRight(working[0], 13) ^ rotateRight(working[0], 22);
            const auto temporary1 = working[7] + sum1 + choice + constants[index] + words[index];
            const auto temporary2 = sum0 + majority;
            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temporary1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temporary1 + temporary2;
        }
        for (std::size_t index = 0; index < state.size(); ++index) state[index] += working[index];
    }
    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        result[index * 4] = static_cast<std::uint8_t>(state[index] >> 24);
        result[index * 4 + 1] = static_cast<std::uint8_t>(state[index] >> 16);
        result[index * 4 + 2] = static_cast<std::uint8_t>(state[index] >> 8);
        result[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
    }
    return result;
}

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

std::string lower(std::string value) {
    for (auto& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

std::string normalized(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return lower(value.substr(first, last - first + 1));
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw MiniSqlError(ErrorCode::Storage, "Cannot open access catalog: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path accessPagesPath(const std::filesystem::path& databasePath) {
    if (const auto* configured = std::getenv("MINISQL_ACCESS_FILE"); configured && *configured) {
        auto path = std::filesystem::path(configured);
        if (path.extension() == ".json") path.replace_extension();
        if (path.extension() != ".pages") path += ".pages";
        return path;
    }
    return databasePath.parent_path() / "access.catalog.pages";
}

bool constantTimeEqual(std::string_view actual, std::string_view expected) {
    if (actual.size() != expected.size()) return false;
    unsigned difference = 0;
    for (std::size_t index = 0; index < actual.size(); ++index)
        difference |= static_cast<unsigned>(static_cast<unsigned char>(actual[index]) ^ static_cast<unsigned char>(expected[index]));
    return difference == 0;
}

bool permissionIn(const nlohmann::json& grants, const std::string& permission, const std::string& object) {
    if (!grants.is_array()) return false;
    for (const auto& grant : grants) {
        if (!grant.is_object()) continue;
        const auto grantObject = normalized(grant.value("object", "*"));
        if (grantObject != "*" && grantObject != object) continue;
        const auto permissions = grant.value("permissions", nlohmann::json::array());
        if (!permissions.is_array()) continue;
        for (const auto& item : permissions) {
            if (!item.is_string()) continue;
            const auto value = normalized(item.get<std::string>());
            if (value == "*" || value == permission) return true;
        }
    }
    return false;
}

bool canPermission(const nlohmann::json& catalog, const std::string& user,
                   const std::string& permission, const std::string& object) {
    const auto users = catalog.value("users", nlohmann::json::object());
    if (!users.is_object()) return false;
    const auto userFound = users.find(normalized(user));
    if (userFound == users.end() || !userFound->is_object()) return false;
    if (permissionIn(userFound->value("grants", nlohmann::json::array()), permission, object)) return true;
    const auto roles = catalog.value("roles", nlohmann::json::object());
    if (!roles.is_object()) return false;
    std::unordered_set<std::string> visited;
    const std::function<bool(const std::string&)> visit = [&](const std::string& roleName) {
        const auto role = normalized(roleName);
        if (!visited.insert(role).second) return false;
        const auto found = roles.find(role);
        if (found == roles.end() || !found->is_object()) return false;
        if (permissionIn(found->value("grants", nlohmann::json::array()), permission, object)) return true;
        const auto parents = found->value("inherits", nlohmann::json::array());
        if (!parents.is_array()) return false;
        for (const auto& parent : parents) if (parent.is_string() && visit(parent.get<std::string>())) return true;
        return false;
    };
    const auto assigned = userFound->value("roles", nlohmann::json::array());
    if (!assigned.is_array()) return false;
    for (const auto& role : assigned) if (role.is_string() && visit(role.get<std::string>())) return true;
    return false;
}

} // namespace

AccessCatalog AccessCatalog::load(const std::filesystem::path& databasePath) {
    AccessCatalog result;
    const auto path = accessPagesPath(databasePath);
    if (!std::filesystem::exists(path)) return result;
    const auto bytes = readFile(path);
    if (bytes.size() < pageSize * 2 || bytes.size() % pageSize != 0)
        throw MiniSqlError(ErrorCode::Storage, "Access catalog is not a whole number of pages");
    if (std::string(bytes.begin(), bytes.begin() + 8) != std::string("MSQLACL\0", 8))
        throw MiniSqlError(ErrorCode::Storage, "Access catalog magic mismatch");
    if (read32(bytes, 8) != 1 || read32(bytes, 12) != pageSize)
        throw MiniSqlError(ErrorCode::Storage, "Unsupported access catalog format");
    const auto pageCount = read32(bytes, 16);
    const auto payloadBytes = read32(bytes, 20);
    result.permissionVersion_ = read32(bytes, 24);
    if (pageCount != bytes.size() / pageSize || pageCount < 2 || payloadBytes > (pageCount - 1) * dataCapacity)
        throw MiniSqlError(ErrorCode::Storage, "Access catalog page metadata is invalid");
    std::vector<std::uint8_t> payload;
    payload.reserve(payloadBytes);
    for (std::uint32_t page = 1; page < pageCount; ++page) {
        const auto offset = static_cast<std::size_t>(page) * pageSize;
        if (std::string(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)) != "ACLD")
            throw MiniSqlError(ErrorCode::Storage, "Access catalog data page magic mismatch");
        if (read32(bytes, offset + 4) != page) throw MiniSqlError(ErrorCode::Storage, "Access catalog data page id mismatch");
        const auto length = read32(bytes, offset + 8);
        if (length > dataCapacity || read32(bytes, offset + 12) != fnv1a(bytes.data() + offset + dataHeaderBytes, length))
            throw MiniSqlError(ErrorCode::Storage, "Access catalog data page checksum mismatch");
        payload.insert(payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + dataHeaderBytes),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + dataHeaderBytes + length));
    }
    if (payload.size() != payloadBytes) throw MiniSqlError(ErrorCode::Storage, "Access catalog payload length mismatch");
    const auto expected = std::string(bytes.begin() + 40, bytes.begin() + 72);
    const auto actual = sha256(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    if (!constantTimeEqual(std::string(reinterpret_cast<const char*>(actual.data()), actual.size()), expected)) {
        // META 中保存的是原始 SHA-256 字节，而密码记录保存的是十六进制文本；这里按字节比较摘要。
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload digest mismatch");
    }
    try {
        result.catalog_ = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    } catch (const nlohmann::json::exception&) {
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload is not valid JSON");
    }
    if (!result.catalog_.is_object() || !result.catalog_.contains("users") || !result.catalog_.contains("roles"))
        throw MiniSqlError(ErrorCode::Storage, "Access catalog payload is missing users or roles");
    result.enabled_ = true;
    return result;
}

AccessCatalog AccessCatalog::fromDocument(nlohmann::json document, std::uint32_t permissionVersion) {
    if (permissionVersion == 0 || !document.is_object() || !document.contains("users") || !document.contains("roles") ||
        !document.at("users").is_object() || !document.at("roles").is_object())
        throw MiniSqlError(ErrorCode::Storage, "Access catalog document is invalid");
    AccessCatalog result;
    result.enabled_ = true;
    result.permissionVersion_ = permissionVersion;
    result.catalog_ = std::move(document);
    return result;
}

bool AccessCatalog::verify(const std::string& user, const std::string& password) const {
    if (!enabled_) return true;
    const auto name = normalized(user);
    const auto users = catalog_.value("users", nlohmann::json::object());
    const auto found = users.is_object() ? users.find(name) : users.end();
    if (found == users.end() || !found->is_object()) return false;
    const nlohmann::json hash = found->contains("hash") ? found->at("hash") : nlohmann::json(nullptr);
    if (hash.is_null()) return password.empty();
    if (!hash.is_object() || hash.value("scheme", "") != "sha256-salted") return false;
    const auto salt = hash.value("salt", "");
    const auto expected = lower(hash.value("digest", ""));
    return expected.size() == 64 && constantTimeEqual(hex(sha256(salt + ":" + password)), expected);
}

std::string permissionName(AccessAction action) {
    switch (action) {
        case AccessAction::Connect: return "connect";
        case AccessAction::Compile: return "compile";
        case AccessAction::Read: return "read";
        case AccessAction::Select: return "select";
        case AccessAction::Insert: return "insert";
        case AccessAction::Update: return "update";
        case AccessAction::Delete: return "delete";
        case AccessAction::Create: return "create";
        case AccessAction::Drop: return "drop";
        case AccessAction::Transaction: return "transaction";
        case AccessAction::Checkpoint: return "checkpoint";
    }
    return "compile";
}

void AccessCatalog::authorize(const std::string& user, const std::string& operation, const AccessRequest& request,
                              const std::string& table, const std::string& index) const {
    if (!enabled_) return;
    const auto mode = normalized(operation);
    (void)index;

    // 入口动作优先：这些请求不携带 SQL，权限只由入口决定。
    if (mode == "snapshot" || mode == "restore") return requireAll(user, "checkpoint", {"*"});
    if (mode == "close") return requireAll(user, "connect", {"*"});
    if (mode == "catalog" || mode == "statistics" || mode == "buffer") return requireAll(user, "read", {"*"});
    if (mode == "indexinspect")
        return requireAll(user, "read", table.empty() ? std::vector<std::string>{"*"} : std::vector<std::string>{normalized(table)});

    // X25 fail-closed：绑定器没有给出闭合的对象集合就一律拒绝。此处不存在、
    // 也不允许存在任何基于 SQL 文本的兜底扫描。
    if (!request.bound)
        throw MiniSqlError(ErrorCode::Permission,
            request.diagnostic.empty() ? "Permission denied: statement could not be bound"
                                       : "Permission denied: statement could not be bound (" + request.diagnostic + ")");

    // 只编译不执行时，读动作降级为 compile 权限；写动作不降级。
    const bool compileOnly = mode == "compile" || mode == "diagnostics";
    const auto effective = [&](AccessAction action) {
        if (compileOnly && (action == AccessAction::Select || action == AccessAction::Read)) return std::string("compile");
        return permissionName(action);
    };

    if (request.objects.empty()) return requireAll(user, effective(request.statementAction), {"*"});
    for (const auto& object : request.objects)
        if (!canPermission(catalog_, user, effective(object.action), object.object))
            throw MiniSqlError(ErrorCode::Permission, "Permission denied");
}

void AccessCatalog::requireAll(const std::string& user, const std::string& permission,
                               const std::vector<std::string>& objects) const {
    for (const auto& object : objects)
        if (!canPermission(catalog_, user, permission, object))
            throw MiniSqlError(ErrorCode::Permission, "Permission denied");
}

} // namespace minisql::security
