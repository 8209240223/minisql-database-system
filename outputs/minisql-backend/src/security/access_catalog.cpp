#include "minisql/security/access_catalog.hpp"
#include "minisql/common/error.hpp"
#include "minisql/sql/lexer.hpp"
#include "minisql/sql/parser.hpp"

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

std::string firstKeyword(std::string_view source) {
    std::size_t index = 0;
    for (;;) {
        while (index < source.size() && std::isspace(static_cast<unsigned char>(source[index]))) ++index;
        if (index + 1 < source.size() && source[index] == '-' && source[index + 1] == '-') {
            index = source.find('\n', index + 2);
            if (index == std::string_view::npos) return {};
            continue;
        }
        if (index + 1 < source.size() && source[index] == '/' && source[index + 1] == '*') {
            const auto end = source.find("*/", index + 2);
            if (end == std::string_view::npos) return {};
            index = end + 2;
            continue;
        }
        const auto begin = index;
        while (index < source.size() && std::isalpha(static_cast<unsigned char>(source[index]))) ++index;
        return begin == index ? std::string{} : lower(std::string(source.substr(begin, index - begin)));
    }
}

std::vector<std::string> sqlWords(std::string_view source) {
    std::vector<std::string> words;
    const auto alpha = [](char value) { return std::isalpha(static_cast<unsigned char>(value)) || value == '_'; };
    const auto digit = [](char value) { return std::isdigit(static_cast<unsigned char>(value)); };
    for (std::size_t index = 0; index < source.size();) {
        const char character = source[index];
        if (std::isspace(static_cast<unsigned char>(character))) { ++index; continue; }
        if (index + 1 < source.size() && source[index] == '-' && source[index + 1] == '-') {
            index += 2;
            while (index < source.size() && source[index] != '\n' && source[index] != '\r') ++index;
            continue;
        }
        if (index + 1 < source.size() && source[index] == '/' && source[index + 1] == '*') {
            index += 2;
            while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/')) ++index;
            index = std::min(source.size(), index + 2);
            continue;
        }
        if (character == '\'') {
            ++index;
            while (index < source.size()) {
                if (source[index] != '\'') { ++index; continue; }
                if (index + 1 < source.size() && source[index + 1] == '\'') { index += 2; continue; }
                ++index;
                break;
            }
            continue;
        }
        if (alpha(character)) {
            const auto begin = index++;
            while (index < source.size() && (alpha(source[index]) || digit(source[index]))) ++index;
            words.push_back(lower(std::string(source.substr(begin, index - begin))));
            continue;
        }
        words.emplace_back(1, character);
        ++index;
    }
    return words;
}

std::vector<std::string> tableReferences(std::string_view source, const std::string& keyword) {
    const auto words = sqlWords(source);
    std::vector<std::string> result;
    std::unordered_set<std::string> ctes;
    const auto identifier = [](const std::string& value) {
        return !value.empty() && (std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_') &&
            std::all_of(value.begin() + 1, value.end(), [](char character) {
                return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
            });
    };
    const auto add = [&](const std::string& value) {
        if (identifier(value) && !ctes.contains(value) && std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
    };
    const auto addAfter = [&](std::size_t index, bool allowParenthesized) {
        auto cursor = index + 1;
        if (allowParenthesized && cursor < words.size() && words[cursor] == "(") return;
        if (cursor < words.size() && words[cursor] == "lateral") ++cursor;
        if (cursor < words.size()) add(words[cursor]);
    };
    if (!words.empty() && words.front() == "with") {
        std::size_t cursor = words.size() > 1 && words[1] == "recursive" ? 2 : 1;
        for (;;) {
            if (cursor >= words.size() || !identifier(words[cursor])) break;
            ctes.insert(words[cursor++]);
            if (cursor + 1 >= words.size() || words[cursor] != "as" || words[cursor + 1] != "(") break;
            cursor += 2;
            std::size_t depth = 1;
            while (cursor < words.size() && depth) {
                if (words[cursor] == "(") ++depth;
                else if (words[cursor] == ")") --depth;
                ++cursor;
            }
            if (cursor >= words.size() || words[cursor] != ",") break;
            ++cursor;
        }
    }
    const auto effectiveKeyword = keyword == "explain"
        ? std::find_if(words.begin(), words.end(), [](const std::string& value) {
            return value == "select" || value == "insert" || value == "update" || value == "delete";
        })
        : words.end();
    const auto command = keyword == "explain" && effectiveKeyword != words.end() ? *effectiveKeyword : keyword;
    for (std::size_t index = 0; index < words.size(); ++index) {
        const auto& word = words[index];
        if (word == "from" || word == "join" || word == "into" || word == "update" || word == "references") addAfter(index, true);
        if (command == "drop" && word == "table") addAfter(index, false);
        if (command == "create" && (word == "table" || word == "on")) addAfter(index, false);
    }
    return result;
}

bool sqlIdentifier(std::string_view value) {
    if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_')) return false;
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
    });
}

void addAstObject(const std::string& value, std::vector<std::string>& result,
                 std::unordered_set<std::string>& seen) {
    const auto object = normalized(value);
    if (sqlIdentifier(object) && seen.insert(object).second) result.push_back(object);
}

void collectAstStatementObjects(const sql::Statement& statement, std::vector<std::string>& result,
                                std::unordered_set<std::string>& seen);

void collectAstExpressionObjects(const std::shared_ptr<sql::Expr>& expression,
                                 std::vector<std::string>& result,
                                 std::unordered_set<std::string>& seen) {
    if (!expression) return;
    collectAstExpressionObjects(expression->left, result, seen);
    collectAstExpressionObjects(expression->right, result, seen);
    if (expression->subquery) collectAstStatementObjects(*expression->subquery, result, seen);
    else if (!expression->subquerySql.empty()) {
        try {
            auto nestedSql = expression->subquerySql;
            if (nestedSql.find_last_not_of(" \t\r\n") == std::string::npos ||
                nestedSql[nestedSql.find_last_not_of(" \t\r\n")] != ';') nestedSql += ';';
            for (const auto& nested : sql::parse(sql::tokenize(nestedSql)))
                collectAstStatementObjects(nested, result, seen);
        } catch (const MiniSqlError&) {
            // The caller will use the lexical fallback when the complete source cannot be parsed.
        }
    }
}

void collectAstStatementObjects(const sql::Statement& statement, std::vector<std::string>& result,
                                std::unordered_set<std::string>& seen) {
    // A derived-table alias is a scope name, not a database object. Its nested statement is collected below.
    if (!statement.table.empty() && !(statement.kind == "Select" && statement.fromSubquery))
        addAstObject(statement.table, result, seen);
    for (const auto& join : statement.joins) addAstObject(join.table, result, seen);
    for (const auto& foreignKey : statement.foreignKeys) addAstObject(foreignKey.table, result, seen);
    for (const auto& column : statement.columns)
        if (column.references) addAstObject(column.references->first, result, seen);
    if (statement.fromSubquery) collectAstStatementObjects(*statement.fromSubquery, result, seen);
    collectAstExpressionObjects(statement.where, result, seen);
    collectAstExpressionObjects(statement.having, result, seen);
    for (const auto& item : statement.selectItems) collectAstExpressionObjects(item.expression, result, seen);
    for (const auto& item : statement.orderBy) collectAstExpressionObjects(item.expression, result, seen);
    for (const auto& item : statement.assignments) collectAstExpressionObjects(item.expression, result, seen);
    for (const auto& item : statement.groupBy) collectAstExpressionObjects(item, result, seen);
    for (const auto& item : statement.checks) collectAstExpressionObjects(item, result, seen);
    for (const auto& item : statement.valueExpressions) collectAstExpressionObjects(item, result, seen);
    for (const auto& row : statement.valueRows)
        for (const auto& item : row) collectAstExpressionObjects(item, result, seen);
}

std::vector<std::string> astTableReferences(std::string_view source, const std::string& keyword) {
    try {
        auto tokens = sql::tokenize(std::string(source));
        if (keyword == "explain" && !tokens.empty()) {
            const auto lowerLexeme = [](const sql::Token& token) { return normalized(token.lexeme); };
            if (lowerLexeme(tokens.front()) == "explain") tokens.erase(tokens.begin());
            if (!tokens.empty() && lowerLexeme(tokens.front()) == "analyze") tokens.erase(tokens.begin());
        }
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        for (const auto& statement : sql::parse(tokens)) collectAstStatementObjects(statement, result, seen);
        return result.empty() ? tableReferences(source, keyword) : result;
    } catch (const MiniSqlError&) {
        // Unsupported or malformed syntax is still checked by the conservative scanner before execution.
        return tableReferences(source, keyword);
    }
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

void AccessCatalog::authorize(const std::string& user, const std::string& operation, const std::string& sql,
                              const std::string& table, const std::string& index) const {
    if (!enabled_) return;
    const auto mode = normalized(operation);
    const auto keyword = firstKeyword(sql);
    std::string permission = "compile";
    if (mode == "catalog" || mode == "statistics" || mode == "buffer") permission = "read";
    else if (mode == "close") permission = "connect";
    else if (mode == "indexinspect") permission = "read";
    else if (keyword == "begin" || keyword == "commit" || keyword == "rollback" || keyword == "checkpoint") permission = "transaction";
    else if (keyword == "create") permission = "create";
    else if (keyword == "drop") permission = "drop";
    else if (keyword == "select") permission = mode == "compile" || mode == "diagnostics" ? "compile" : "select";
    else if (keyword == "insert" || keyword == "update" || keyword == "delete") permission = keyword;
    std::vector<std::string> objects;
    if (mode == "indexinspect" && !table.empty()) objects.push_back(normalized(table));
    else objects = astTableReferences(sql, keyword);
    if (objects.empty()) objects.push_back("*");
    (void)index;
    for (const auto& object : objects)
        if (!canPermission(catalog_, user, permission, object)) throw MiniSqlError(ErrorCode::Permission, "Permission denied");
}

} // namespace minisql::security
