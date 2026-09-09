#include "minisql/common/config.hpp"
#include "minisql/common/error.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>
#include <map>

namespace minisql {
using test::TempDirectory;
TEST(Config, DefaultsAreValidAndRoundTrip) {
    const auto defaults = loadConfig(std::nullopt, nlohmann::json::object(), test::emptyEnvironment());
    EXPECT_EQ(defaults.pageSize, 4096u);
    EXPECT_EQ(defaults.server.port, 8080);
    EXPECT_EQ(defaults.mode, "cli");
    EXPECT_NO_THROW(defaults.validate());
    EXPECT_EQ(loadConfig(std::nullopt, defaults.toJson(), test::emptyEnvironment()).toJson(), defaults.toJson());
}
TEST(Config, PrecedenceIsCliThenEnvironmentThenFileThenDefaults) {
    TempDirectory directory;
    auto file = directory.write("config.json", R"({"server":{"port":8001,"host":"localhost"},"logging":{"level":"warn"}})");
    Environment env = [](const std::string& name) -> std::optional<std::string> {
        if (name == "MINISQL_PORT") return "8002";
        if (name == "MINISQL_PAGE_SIZE") return "8192";
        return std::nullopt;
    };
    const auto fromEnv = loadConfig(file, nlohmann::json::object(), env);
    EXPECT_EQ(fromEnv.server.port, 8002);
    const auto config = loadConfig(file, {{"server", {{"port", 8003}}}}, env);
    EXPECT_EQ(config.server.port, 8003);
    EXPECT_EQ(config.server.host, "localhost");
    EXPECT_EQ(config.pageSize, 8192u);
    EXPECT_EQ(config.bufferPoolSize, 128u);
    EXPECT_EQ(config.logging.level, "WARN");
}
TEST(Config, MissingExplicitFileIsError) {
    TempDirectory directory;
    EXPECT_THROW(loadConfig(directory.path() / "missing.json", {}, test::emptyEnvironment()), MiniSqlError);
}
TEST(Config, UnicodeConfigFileName) {
    TempDirectory directory;
    const auto file = directory.write("测试配置.json", R"({"page_size":8192})");
    EXPECT_EQ(loadConfig(file, nlohmann::json::object(), test::emptyEnvironment()).pageSize, 8192u);
}
TEST(Config, RejectsMalformedAndTrailingJson) {
    TempDirectory directory;
    for (const auto* contents : {"{", "", "{} trailing", "{} {}"}) {
        auto file = directory.write("bad.json", contents);
        EXPECT_THROW(loadConfig(file, nlohmann::json::object(), test::emptyEnvironment()), MiniSqlError);
    }
}
class InvalidConfig : public ::testing::TestWithParam<const char*> {};
TEST_P(InvalidConfig, RejectsInvalidShapeOrRange) {
    const auto json = nlohmann::json::parse(GetParam());
    EXPECT_THROW(loadConfig(std::nullopt, json, test::emptyEnvironment()), MiniSqlError);
}
INSTANTIATE_TEST_SUITE_P(Validation, InvalidConfig, ::testing::Values(
    R"({"unknown":1})", R"({"server":{"prot":80}})", R"({"server":null})",
    R"({"page_size":"4096"})", R"({"page_size":4096.0})", R"({"page_size":3000})",
    R"({"page_size":-4096})", R"({"buffer_pool_size":0})", R"({"server":{"port":0}})",
    R"({"server":{"port":65536}})", R"({"server":{"port":18446744073709551615}})",
    R"({"server":{"websocket_enabled":"false"}})", R"({"logging":{"level":"garbage"}})",
    R"({"logging":{"max_file_size":0}})", R"({"logging":{"max_files":0}})",
    R"({"data_directory":""})", R"({"data_directory":"a\u0000b"})",
    R"({"replacement_policy":"random"})", R"({"mode":"other"})", "[]"));
TEST(Config, EnvironmentRejectsGarbageNumbersAndBoolean) {
    for (const auto* value : {"", "8080abc", "3.5", "99999999999999999999999999"}) {
        Environment env = [=](const std::string& name) -> std::optional<std::string> {
            if (name == "MINISQL_PORT") return value;
            return std::nullopt;
        };
        EXPECT_THROW(loadConfig(std::nullopt, nlohmann::json::object(), env), MiniSqlError);
    }
    Environment badBool = [](const std::string& name) -> std::optional<std::string> {
        if (name == "MINISQL_LOG_CONSOLE") return "maybe";
        return std::nullopt;
    };
    EXPECT_THROW(loadConfig(std::nullopt, nlohmann::json::object(), badBool), MiniSqlError);
}
TEST(Config, EnvironmentSupportsPathsAndBooleans) {
    const std::map<std::string, std::string> values{
        {"MINISQL_WAL_DIRECTORY", "./other/wal"}, {"MINISQL_LOG_CONSOLE", "false"},
        {"MINISQL_WEBSOCKET_ENABLED", "0"}, {"MINISQL_LOG_LEVEL", "debug"}};
    Environment env = [&](const std::string& name) -> std::optional<std::string> {
        auto found = values.find(name);
        return found == values.end() ? std::nullopt : std::optional(found->second);
    };
    const auto config = loadConfig(std::nullopt, nlohmann::json::object(), env);
    EXPECT_EQ(config.walDirectory, "./other/wal");
    EXPECT_FALSE(config.logging.console);
    EXPECT_FALSE(config.server.websocketEnabled);
    EXPECT_EQ(config.logging.level, "DEBUG");
}
TEST(Config, CreatesUnicodeDirectoriesAndRejectsFileCollision) {
    TempDirectory directory;
    Config config;
    config.dataDirectory = test::utf8(directory.path() / pathFromUtf8("数据"));
    config.walDirectory = config.dataDirectory + "/wal";
    config.catalogDirectory = config.dataDirectory + "/system";
    config.logging.directory = test::utf8(directory.path() / "logs");
    prepareRuntimeDirectories(config);
    EXPECT_TRUE(std::filesystem::is_directory(pathFromUtf8(config.walDirectory)));
    EXPECT_NO_THROW(prepareRuntimeDirectories(config));
    config.walDirectory = test::utf8(directory.write("occupied", "file"));
    EXPECT_THROW(prepareRuntimeDirectories(config), MiniSqlError);
}
} // namespace minisql
