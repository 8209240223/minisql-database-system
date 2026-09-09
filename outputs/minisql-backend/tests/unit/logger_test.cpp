#include "minisql/common/logger.hpp"
#include "minisql/common/error.hpp"
#include "test_support.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <thread>
#include <vector>

namespace minisql {
TEST(Logger, FiltersLevelsAndIncludesContext) {
    test::TempDirectory directory;
    LogConfig config;
    config.directory = test::utf8(directory.path());
    config.console = false;
    {
        LogManager log(config);
        log.log("sql", spdlog::level::debug, "hidden-debug");
        log.log("sql", spdlog::level::info, "visible-info", {"req-42", "txn-3"});
        log.flush();
    }
    const auto text = test::read(directory.path() / "sql.log");
    EXPECT_EQ(text.find("hidden-debug"), std::string::npos);
    EXPECT_NE(text.find("visible-info"), std::string::npos);
    EXPECT_NE(text.find("[sql]"), std::string::npos);
    EXPECT_NE(text.find("thread="), std::string::npos);
    EXPECT_NE(text.find("request=req-42"), std::string::npos);
    EXPECT_NE(text.find("txn=txn-3"), std::string::npos);
    EXPECT_TRUE(test::read(directory.path() / "storage.log").empty());
}
TEST(Logger, RotationRetainsBoundedFilesAndLastMessage) {
    test::TempDirectory directory;
    LogConfig config;
    config.directory = test::utf8(directory.path());
    config.console = false;
    config.maxFileSize = 1024;
    config.maxFiles = 2;
    {
        LogManager log(config);
        for (int i = 0; i < 100; ++i) { log.log("storage", spdlog::level::info, std::string(200, 'x')); }
        log.log("storage", spdlog::level::info, "last-message");
    }
    EXPECT_TRUE(std::filesystem::exists(directory.path() / "storage.1.log"));
    EXPECT_TRUE(std::filesystem::exists(directory.path() / "storage.2.log"));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "storage.3.log"));
    EXPECT_NE(test::read(directory.path() / "storage.log").find("last-message"), std::string::npos);
}
TEST(Logger, ConcurrentWritersDoNotLoseRecords) {
    test::TempDirectory directory;
    LogConfig config;
    config.directory = test::utf8(directory.path());
    config.console = false;
    {
        LogManager log(config);
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&log, i] {
                for (int j = 0; j < 100; ++j) {
                    log.log("transaction", spdlog::level::info, "record", {std::to_string(i), std::to_string(j)});
                }
            });
        }
        for (auto& thread : threads) { thread.join(); }
    }
    auto contents = test::read(directory.path() / "transaction.log");
    EXPECT_EQ(std::count(contents.begin(), contents.end(), '\n'), 400);
}
TEST(Logger, EscapesLineBreaksAndRejectsUnknownModules) {
    test::TempDirectory directory;
    LogConfig config;
    config.directory = test::utf8(directory.path());
    config.console = false;
    {
        LogManager log(config);
        log.log("common", spdlog::level::info, "one\ntwo", {"r\nf", "-"});
        EXPECT_THROW(log.log("unknown", spdlog::level::info, "no"), MiniSqlError);
    }
    const auto contents = test::read(directory.path() / "common.log");
    EXPECT_EQ(std::count(contents.begin(), contents.end(), '\n'), 1);
    EXPECT_NE(contents.find("one\\ntwo"), std::string::npos);
}
TEST(Logger, FileCollisionFailsWithStorageError) {
    test::TempDirectory directory;
    LogConfig config;
    config.directory = test::utf8(directory.write("not-directory", "file"));
    config.console = false;
    EXPECT_THROW((LogManager{config}), MiniSqlError);
}
TEST(Logger, UnicodeDirectoryAndMessageArePreserved) {
    test::TempDirectory directory;
    const auto logPath = directory.path() / pathFromUtf8("日志目录");
    LogConfig config;
    config.directory = test::utf8(logPath);
    config.console = false;
    {
        LogManager log(config);
        log.log("common", spdlog::level::info, "日志测试");
    }
    EXPECT_NE(test::read(logPath / "common.log").find("日志测试"), std::string::npos);
}
} // namespace minisql
