#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "trafficmcmt/core/Result.h"

namespace {

using tmcmt::ErrorCode;
using tmcmt::Result;
using tmcmt::Status;

TEST(Status, DefaultIsOk) {
    const Status status;
    EXPECT_TRUE(status.ok());
    EXPECT_TRUE(static_cast<bool>(status));
    EXPECT_EQ(status.code(), ErrorCode::Ok);
    EXPECT_EQ(status.ToString(), "ok");
}

TEST(Status, ErrorKeepsCodeAndMessage) {
    const Status status = Status::Error(ErrorCode::ParseError, "bad yaml");
    EXPECT_FALSE(status.ok());
    EXPECT_FALSE(static_cast<bool>(status));
    EXPECT_EQ(status.code(), ErrorCode::ParseError);
    EXPECT_EQ(status.message(), "bad yaml");
    EXPECT_EQ(status.ToString(), "parse_error: bad yaml");
}

TEST(Result, HoldsValue) {
    Result<int> result = 42;
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), 42);
    EXPECT_EQ(result.value_or(7), 42);
    EXPECT_TRUE(result.status().ok());
}

TEST(Result, HoldsError) {
    Result<int> result = Status::Error(ErrorCode::NotFound, "missing");
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.status().code(), ErrorCode::NotFound);
    EXPECT_EQ(result.value_or(7), 7);
}

TEST(Result, MovesValueOut) {
    Result<std::string> result = std::string("tracklet");
    std::string value = std::move(result).value();
    EXPECT_EQ(value, "tracklet");
}

TEST(Result, MovesNonCopyableValue) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(5);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 5);
}

} // namespace
