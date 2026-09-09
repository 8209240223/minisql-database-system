#include "minisql/common/error.hpp"
#include "minisql/common/types.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace minisql {
TEST(Error, SerializesDiagnosticLocationAndSuggestion) {
    MiniSqlError error(ErrorCode::Syntax, "Expected FROM", {2, 7}, "Add FROM before table");
    const auto value = error.toJson();
    EXPECT_FALSE(value.at("success").get<bool>());
    EXPECT_EQ(value["error"]["code"], 2002);
    EXPECT_EQ(value["error"]["type"], "SyntaxError");
    EXPECT_EQ(value["error"]["line"], 2);
    EXPECT_EQ(value["error"]["column"], 7);
    EXPECT_EQ(value["error"]["suggestion"], "Add FROM before table");
    EXPECT_STREQ(error.what(), "Expected FROM");
}
TEST(Error, StatusSuccessAndFailure) {
    EXPECT_TRUE(Status{}.ok());
    EXPECT_NO_THROW(Status{}.throwIfError());
    EXPECT_THROW((Status{ErrorCode::Storage, "Disk failed"}.throwIfError()), MiniSqlError);
    EXPECT_THROW((Status{ErrorCode::Ok, "contradiction"}), std::invalid_argument);
}
TEST(Error, ResultPreservesValueAndError) {
    Result<std::string> success(std::string("value"));
    EXPECT_TRUE(success.ok());
    EXPECT_EQ(success.value(), "value");
    EXPECT_TRUE(success.status().ok());
    Result<int> failure(Status{ErrorCode::Catalog, "No table"});
    EXPECT_FALSE(failure.ok());
    EXPECT_EQ(failure.status().code(), ErrorCode::Catalog);
    EXPECT_THROW(failure.value(), MiniSqlError);
    EXPECT_THROW((Result<int>{Status{}}), std::invalid_argument);
}
TEST(Types, RidHasExplicitInvalidDefault) {
    EXPECT_EQ(Rid{}.pageId, kInvalidPageId);
    EXPECT_EQ((Rid{1, 2}), (Rid{1, 2}));
    EXPECT_NE((Rid{1, 2}), (Rid{1, 3}));
}
} // namespace minisql
