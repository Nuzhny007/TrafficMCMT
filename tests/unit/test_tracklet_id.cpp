#include <gtest/gtest.h>

#include <string>

#include "trafficmcmt/core/TrackletId.h"
#include "trafficmcmt/core/Types.h"

#include "TrackletTestHelpers.h"

namespace {

using tmcmt::AssignTrackletId;
using tmcmt::MakeTrackletId;
using tmcmt::MakeTrackletKey;
using tmcmt::TrackletKey;
using tmcmt::TrackletKeyHash;

TEST(TrackletId, IsStableAndNonZero) {
    const auto first = MakeTrackletId("session", "cam-01", 42, 3);
    const auto second = MakeTrackletId("session", "cam-01", 42, 3);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, 0U);
}

TEST(TrackletId, DependsOnEveryIdentityField) {
    const auto base = MakeTrackletId("session", "cam-01", 42, 3);

    EXPECT_NE(base, MakeTrackletId("other", "cam-01", 42, 3));
    EXPECT_NE(base, MakeTrackletId("session", "cam-02", 42, 3));
    EXPECT_NE(base, MakeTrackletId("session", "cam-01", 43, 3));
    EXPECT_NE(base, MakeTrackletId("session", "cam-01", 42, 4));
}

TEST(TrackletId, AssignsIdFromIdentity) {
    tmcmt::Tracklet tracklet = tmcmt::test::MakeTracklet("session-x", "cam-07", 11, 5);
    tracklet.id = 0;
    AssignTrackletId(tracklet);

    EXPECT_EQ(tracklet.id, MakeTrackletId("session-x", "cam-07", 11, 5));
}

TEST(TrackletId, KeyMatchesTracklet) {
    const tmcmt::Tracklet tracklet = tmcmt::test::MakeTracklet("session-x", "cam-07", 11, 5);
    const TrackletKey key = MakeTrackletKey(tracklet);

    EXPECT_EQ(key.session_id, "session-x");
    EXPECT_EQ(key.camera, "cam-07");
    EXPECT_EQ(key.local_track_id, 11U);
    EXPECT_EQ(key.sequence, 5U);
}

TEST(TrackletId, KeyHashIsConsistentWithEquality) {
    const TrackletKey lhs{"session", "cam", 1, 2};
    const TrackletKey rhs{"session", "cam", 1, 2};
    const TrackletKey other{"session", "cam", 1, 3};

    EXPECT_TRUE(lhs == rhs);
    EXPECT_FALSE(lhs == other);
    EXPECT_EQ(TrackletKeyHash{}(lhs), TrackletKeyHash{}(rhs));
}

} // namespace
