#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "trafficmcmt/io/InMemoryTrackletStore.h"

#include "TrackletTestHelpers.h"

namespace {

using tmcmt::InMemoryTrackletStore;
using tmcmt::TrackletQuery;

TEST(InMemoryTrackletStore, AddsAndGetsTracklets) {
    InMemoryTrackletStore store;
    const tmcmt::Tracklet tracklet = tmcmt::test::MakeTracklet();

    ASSERT_TRUE(store.Add(tracklet).ok());
    EXPECT_EQ(store.Size(), 1U);

    auto found = store.Get(tracklet.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->id, tracklet.id);
    EXPECT_EQ(found->camera, tracklet.camera);
}

TEST(InMemoryTrackletStore, ReplacesTrackletWithSameId) {
    InMemoryTrackletStore store;
    tmcmt::Tracklet tracklet = tmcmt::test::MakeTracklet();
    ASSERT_TRUE(store.Add(tracklet).ok());

    tracklet.quality = 0.1f;
    ASSERT_TRUE(store.Add(tracklet).ok());

    EXPECT_EQ(store.Size(), 1U);
    auto found = store.Get(tracklet.id);
    ASSERT_TRUE(found.has_value());
    EXPECT_FLOAT_EQ(found->quality, 0.1f);
}

TEST(InMemoryTrackletStore, QueriesByCamera) {
    InMemoryTrackletStore store;
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam-01", 1, 0)).ok());
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam-02", 1, 0)).ok());
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam-02", 2, 0)).ok());

    TrackletQuery query;
    query.camera = "cam-02";
    const std::vector<tmcmt::Tracklet> result = store.Query(query);
    ASSERT_EQ(result.size(), 2U);
    for (const tmcmt::Tracklet& tracklet : result) {
        EXPECT_EQ(tracklet.camera, "cam-02");
    }
}

TEST(InMemoryTrackletStore, QueriesByTimeRange) {
    InMemoryTrackletStore store;
    constexpr std::int64_t base = 1'700'000'000'000LL;
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam", 1, 0, base)).ok());
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam", 2, 0, base + 1000)).ok());
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam", 3, 0, base + 2000)).ok());

    TrackletQuery query;
    query.from = tmcmt::Timestamp(base + 500);
    query.to = tmcmt::Timestamp(base + 1500);
    const std::vector<tmcmt::Tracklet> result = store.Query(query);
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result.front().local_track_id, 2U);
}

TEST(InMemoryTrackletStore, AppliesLimit) {
    InMemoryTrackletStore store;
    constexpr std::int64_t base = 1'700'000'000'000LL;
    for (std::uint64_t i = 0; i < 5; ++i) {
        ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam", i, 0, base + static_cast<std::int64_t>(i) * 10))
                        .ok());
    }

    TrackletQuery query;
    query.limit = 2;
    EXPECT_EQ(store.Query(query).size(), 2U);
}

TEST(InMemoryTrackletStore, ClearsAllIndexes) {
    InMemoryTrackletStore store;
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam-01", 1, 0)).ok());
    ASSERT_TRUE(store.Add(tmcmt::test::MakeTracklet("s", "cam-02", 2, 0)).ok());

    store.Clear();
    EXPECT_EQ(store.Size(), 0U);
    EXPECT_TRUE(store.Query(TrackletQuery{}).empty());
}

TEST(InMemoryTrackletStore, ReturnsNulloptForUnknownId) {
    InMemoryTrackletStore store;
    EXPECT_FALSE(store.Get(123456).has_value());
}

} // namespace
