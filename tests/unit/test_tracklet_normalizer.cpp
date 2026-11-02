#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "trafficmcmt/io/TrackletNormalizer.h"

#include "TrackletTestHelpers.h"

namespace {

using tmcmt::NormalizeStatus;
using tmcmt::TrackletNormalizer;

TEST(TrackletNormalizer, AcceptsValidTrackletAndAssignsId) {
    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(tmcmt::test::MakeTracklet());

    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_EQ(result.value().status, NormalizeStatus::Accepted);
    EXPECT_NE(result.value().tracklet.id, 0U);
    EXPECT_EQ(normalizer.SeenCount(), 1U);
}

TEST(TrackletNormalizer, SortsObservationsAndExpandsInterval) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    // Put the observations out of order, then declare an interval that covers
    // neither the earliest nor the latest observation.
    std::swap(input.observations[0], input.observations[1]);
    input.observations[1].ts = input.observations[0].ts + std::chrono::milliseconds(50);
    input.begin = input.observations.front().ts + std::chrono::milliseconds(20);
    input.end = input.observations.back().ts - std::chrono::milliseconds(20);

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    ASSERT_TRUE(result) << result.status().ToString();

    const tmcmt::Tracklet& out = result.value().tracklet;
    ASSERT_EQ(out.observations.size(), 2U);
    EXPECT_LE(out.observations[0].ts, out.observations[1].ts);
    EXPECT_EQ(out.begin, out.observations.front().ts);
    EXPECT_EQ(out.end, out.observations.back().ts);
}

TEST(TrackletNormalizer, NormalizesEmbeddingToUnitLength) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.embedding.values = {2.f, 0.f, 0.f, 0.f};

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_NEAR(result.value().tracklet.embedding.Norm(), 1.f, 1e-6f);
    EXPECT_NEAR(result.value().tracklet.embedding.values[0], 1.f, 1e-6f);
}

TEST(TrackletNormalizer, DetectsDuplicateChunks) {
    TrackletNormalizer normalizer;
    const tmcmt::Tracklet input = tmcmt::test::MakeTracklet();

    auto first = normalizer.Normalize(input);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value().status, NormalizeStatus::Accepted);

    auto second = normalizer.Normalize(input);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().status, NormalizeStatus::Duplicate);
    EXPECT_EQ(normalizer.SeenCount(), 1U);

    normalizer.Reset();
    auto afterReset = normalizer.Normalize(input);
    ASSERT_TRUE(afterReset);
    EXPECT_EQ(afterReset.value().status, NormalizeStatus::Accepted);
}

TEST(TrackletNormalizer, DifferentSequencesAreNotDuplicates) {
    TrackletNormalizer normalizer;
    auto first = normalizer.Normalize(tmcmt::test::MakeTracklet("s", "c", 1, 0));
    auto second = normalizer.Normalize(tmcmt::test::MakeTracklet("s", "c", 1, 1));

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value().status, NormalizeStatus::Accepted);
    EXPECT_EQ(normalizer.SeenCount(), 2U);
}

TEST(TrackletNormalizer, RejectsMissingSessionId) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.session_id.clear();

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(TrackletNormalizer, RejectsUnsupportedSchemaVersion) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.schema_version = 2;

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::Unsupported);
}

TEST(TrackletNormalizer, RejectsEmptyObservations) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.observations.clear();

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(TrackletNormalizer, RejectsInvalidEmbedding) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.embedding.dim = 8;

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code(), tmcmt::ErrorCode::InvalidArgument);
}

TEST(TrackletNormalizer, ClampsQuality) {
    tmcmt::Tracklet input = tmcmt::test::MakeTracklet();
    input.quality = 5.f;

    TrackletNormalizer normalizer;
    auto result = normalizer.Normalize(input);
    ASSERT_TRUE(result) << result.status().ToString();
    EXPECT_FLOAT_EQ(result.value().tracklet.quality, 1.f);
}

} // namespace
