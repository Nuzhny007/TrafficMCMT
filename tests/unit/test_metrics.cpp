#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/metrics/MotMetrics.h"

namespace {

using namespace tmcmt;
using namespace tmcmt::metrics;

Timestamp Ms(std::int64_t value) {
    return Timestamp(value);
}

TrackIdentity Gt(TrackletId id, std::string identity, std::int64_t begin) {
    TrackIdentity entry;
    entry.tracklet_id = id;
    entry.identity = std::move(identity);
    entry.begin = Ms(begin);
    return entry;
}

PredictedIdentity Pred(TrackletId id, GlobalId globalId) {
    return PredictedIdentity{id, globalId};
}

TEST(MotMetricsTest, PerfectAssociation) {
    const std::vector<TrackIdentity> gt{
        Gt(1, "a", 0),
        Gt(2, "a", 100),
        Gt(3, "b", 0),
        Gt(4, "b", 100),
    };
    const std::vector<PredictedIdentity> predicted{
        Pred(1, 10),
        Pred(2, 10),
        Pred(3, 20),
        Pred(4, 20),
    };
    const MetricsReport report = MotMetrics::Compute(gt, predicted);
    EXPECT_EQ(report.gt_tracks, 4u);
    EXPECT_EQ(report.predicted_tracks, 4u);
    EXPECT_EQ(report.idtp, 4u);
    EXPECT_EQ(report.idfp, 0u);
    EXPECT_EQ(report.idfn, 0u);
    EXPECT_EQ(report.idsw, 0u);
    EXPECT_DOUBLE_EQ(report.idf1, 1.0);
    EXPECT_DOUBLE_EQ(report.idp, 1.0);
    EXPECT_DOUBLE_EQ(report.idr, 1.0);
}

TEST(MotMetricsTest, SplitIdentityCountsSwitch) {
    const std::vector<TrackIdentity> gt{
        Gt(1, "a", 0),
        Gt(2, "a", 100),
        Gt(3, "b", 0),
    };
    const std::vector<PredictedIdentity> predicted{
        Pred(1, 10),
        Pred(2, 11),
        Pred(3, 20),
    };
    const MetricsReport report = MotMetrics::Compute(gt, predicted);
    EXPECT_EQ(report.idtp, 2u);
    EXPECT_EQ(report.idfp, 1u);
    EXPECT_EQ(report.idfn, 1u);
    EXPECT_EQ(report.idsw, 1u);
    EXPECT_NEAR(report.idf1, 2.0 * 2.0 / (2.0 * 2.0 + 1.0 + 1.0), 1e-9);
    EXPECT_NEAR(report.idp, 2.0 / 3.0, 1e-9);
    EXPECT_NEAR(report.idr, 2.0 / 3.0, 1e-9);
}

TEST(MotMetricsTest, MergedIdentityLosesOneMatch) {
    const std::vector<TrackIdentity> gt{
        Gt(1, "a", 0),
        Gt(2, "b", 0),
    };
    const std::vector<PredictedIdentity> predicted{
        Pred(1, 10),
        Pred(2, 10),
    };
    const MetricsReport report = MotMetrics::Compute(gt, predicted);
    EXPECT_EQ(report.idtp, 1u);
    EXPECT_EQ(report.idfp, 1u);
    EXPECT_EQ(report.idfn, 1u);
    EXPECT_EQ(report.idsw, 0u);
    EXPECT_NEAR(report.idf1, 0.5, 1e-9);
}

TEST(MotMetricsTest, UnmatchedTracksBecomeFpAndFn) {
    const std::vector<TrackIdentity> gt{
        Gt(1, "a", 0),
        Gt(2, "b", 0),
    };
    const std::vector<PredictedIdentity> predicted{
        Pred(1, 10),
        Pred(3, 30),
    };
    const MetricsReport report = MotMetrics::Compute(gt, predicted);
    EXPECT_EQ(report.idtp, 1u);
    EXPECT_EQ(report.idfp, 1u);
    EXPECT_EQ(report.idfn, 1u);
    EXPECT_NEAR(report.idf1, 0.5, 1e-9);
}

TEST(MotMetricsTest, EmptyInputsReturnZeros) {
    const MetricsReport report = MotMetrics::Compute({}, {});
    EXPECT_EQ(report.idtp, 0u);
    EXPECT_DOUBLE_EQ(report.idf1, 0.0);
    EXPECT_DOUBLE_EQ(report.idp, 0.0);
    EXPECT_DOUBLE_EQ(report.idr, 0.0);
}

TEST(MotMetricsTest, HungarianHandlesRectangularMatrices) {
    const std::vector<std::vector<double>> cost{
        {4.0, 1.0, 3.0},
        {2.0, 0.0, 5.0},
    };
    const std::vector<std::size_t> assignment = MotMetrics::Hungarian(cost);
    ASSERT_EQ(assignment.size(), 2u);
    EXPECT_EQ(assignment[0], 1u);
    EXPECT_EQ(assignment[1], 0u);

    const std::vector<std::vector<double>> tall{
        {1.0, 4.0},
        {2.0, 0.0},
        {3.0, 3.0},
    };
    const std::vector<std::size_t> tallAssignment = MotMetrics::Hungarian(tall);
    ASSERT_EQ(tallAssignment.size(), 3u);
    EXPECT_EQ(tallAssignment[0], 0u);
    EXPECT_EQ(tallAssignment[1], 1u);
    EXPECT_EQ(tallAssignment[2], 2u);
}

} // namespace
