#include "minisql/server/application.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

namespace minisql {
struct RunResult { int code; std::string output; std::string error; };
RunResult run(std::vector<std::string> arguments, const std::string& text = "",
              const Environment& env = test::emptyEnvironment()) {
    arguments.insert(arguments.begin(), "minisql");
    std::vector<const char*> argv;
    for (const auto& argument : arguments) argv.push_back(argument.c_str());
    std::istringstream input(text);
    std::ostringstream output, error;
    const auto code = runApplication(static_cast<int>(argv.size()), argv.data(), input, output, error, env);
    return {code, output.str(), error.str()};
}
TEST(Application, HelpAndVersionDoNotReadConfiguration) {
    Environment broken = [](const std::string&) -> std::optional<std::string> { return "invalid"; };
    auto help = run({"--help"}, "", broken);
    EXPECT_EQ(help.code, 0);
    EXPECT_NE(help.output.find("--check-config"), std::string::npos);
    auto version = run({"--version"}, "", broken);
    EXPECT_EQ(version.code, 0);
    EXPECT_NE(version.output.find("0.1.0"), std::string::npos);
}
TEST(Application, InvalidCliIsNonzero) {
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"--unknown"}, {"--port"}, {"--port", "abc"}, {"--check-config", "--port", "0"}}) {
        auto result = run(args);
        EXPECT_NE(result.code, 0);
        EXPECT_FALSE(result.error.empty());
    }
}
TEST(Application, PrintConfigIsPureJsonAndDoesNotCreateDirectories) {
    test::TempDirectory directory;
    const auto data = directory.path() / "uncreated";
    auto result = run({"--print-config", "--data", test::utf8(data), "--port", "9000", "--no-console"});
    ASSERT_EQ(result.code, 0) << result.error;
    auto json = nlohmann::json::parse(result.output);
    EXPECT_EQ(json["server"]["port"], 9000);
    EXPECT_FALSE(json["logging"]["console"].get<bool>());
    EXPECT_TRUE(result.error.empty());
    EXPECT_FALSE(std::filesystem::exists(data));
}
TEST(Application, ConfigFileEnvironmentAndCliLayerCorrectly) {
    test::TempDirectory directory;
    auto file = directory.write("file.json", R"({"server":{"port":8001}})");
    Environment env = [&](const std::string& name) -> std::optional<std::string> {
        if (name == "MINISQL_CONFIG") return test::utf8(file);
        if (name == "MINISQL_PORT") return "8002";
        return std::nullopt;
    };
    auto result = run({"--print-config", "--port", "8003"}, "", env);
    ASSERT_EQ(result.code, 0) << result.error;
    EXPECT_EQ(nlohmann::json::parse(result.output)["server"]["port"], 8003);
    auto other = directory.write("other.json", R"({"page_size":8192})");
    result = run({"--print-config", "--config", test::utf8(other)}, "", env);
    ASSERT_EQ(result.code, 0) << result.error;
    EXPECT_EQ(nlohmann::json::parse(result.output)["page_size"], 8192);
}
TEST(Application, ExecuteUsesDatabaseCoreAndServerModeIsExplicit) {
    test::TempDirectory directory;
    const auto executed = run({"--no-console", "--data", test::utf8(directory.path()),
        "--wal-dir", test::utf8(directory.path() / "wal"),
        "--catalog-dir", test::utf8(directory.path() / "catalog"),
        "--log-dir", test::utf8(directory.path() / "logs"),
        "--execute", "CREATE TABLE t(id INT); INSERT INTO t VALUES(7); SELECT * FROM t;"});
    ASSERT_EQ(executed.code, 0) << executed.error;
    const auto response = nlohmann::json::parse(executed.output);
    ASSERT_TRUE(response["success"].get<bool>());
    EXPECT_EQ(response["results"].back()["rows"], nlohmann::json::array({nlohmann::json::array({7})}));

    const auto server = run({"--mode", "server"});
    ASSERT_EQ(server.code, 1);
    EXPECT_EQ(nlohmann::json::parse(server.error)["error"]["type"], "NotImplementedError");
}
TEST(Application, CheckConfigReportsMalformedConfiguration) {
    test::TempDirectory directory;
    auto file = directory.write("broken.json", "{");
    auto result = run({"--check-config", "--config", test::utf8(file)});
    ASSERT_EQ(result.code, 1);
    EXPECT_EQ(nlohmann::json::parse(result.error)["error"]["type"], "ConfigurationError");
}
TEST(Application, MissingUnicodeFileRemainsAConfigurationError) {
    test::TempDirectory directory;
    const auto missing = directory.path() / pathFromUtf8("不存在.json");
    auto result = run({"--check-config", "--config", test::utf8(missing)});
    ASSERT_EQ(result.code, 1);
    EXPECT_EQ(nlohmann::json::parse(result.error)["error"]["type"], "ConfigurationError");
}
TEST(Application, ShellCommandsAndSqlExecutionWorkWithoutLeakingSql) {
    test::TempDirectory directory;
    const auto logPath = directory.path() / "logs";
    const auto result = run({"--no-console", "--data", test::utf8(directory.path() / "data"),
        "--wal-dir", test::utf8(directory.path() / "wal"),
        "--catalog-dir", test::utf8(directory.path() / "catalog"), "--log-dir", test::utf8(logPath)},
        ".help\n.version\n.config\nCREATE TABLE secret(value VARCHAR);\nINSERT INTO secret VALUES('secret-password');\nSELECT * FROM secret;\n.quit\n");
    ASSERT_EQ(result.code, 0) << result.error;
    EXPECT_NE(result.output.find(".help  .version"), std::string::npos);
    EXPECT_NE(result.output.find("buffer_pool_size"), std::string::npos);
    EXPECT_NE(result.output.find("secret-password"), std::string::npos);
    EXPECT_EQ(test::read(logPath / "sql.log").find("secret-password"), std::string::npos);
    EXPECT_NE(test::read(logPath / "server.log").find("stopped"), std::string::npos);
}
} // namespace minisql
