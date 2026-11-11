#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/pipeline/ClockSyncEstimator.h"
#include "trafficmcmt/pipeline/MetricsCollector.h"
#include "trafficmcmt/pipeline/ReplayGuard.h"
#include "trafficmcmt/pipeline/TimestampBuffer.h"
#include "trafficmcmt/pipeline/WindowScheduler.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::pipeline {
namespace {

std::int64_t EventTime(int index) {
    static constexpr std::array<std::int64_t, 4> kEvents{100, 200, 300, 400};
    return kEvents[static_cast<std::size_t>(index)];
}

std::string CameraFor(int index) {
    return index % 2 == 0 ? "cam-a" : "cam-b";
}

} // namespace

TEST(TimestampBufferTest, ReleasesEveryPermutationInTheSameOrder) {
    std::array<int, 4> order{0, 1, 2, 3};
    std::vector<int> reference;
    std::size_t permutations = 0;

    do {
        TimestampBuffer<int> buffer(BufferOptions{0, 50'000});
        for (const int index : order) {
            buffer.Push(index, Timestamp(EventTime(index)), CameraFor(index), Timestamp(EventTime(index) + 10));
        }

        // Every camera is silent now, so the watermark jumps to the newest
        // event and the whole buffer becomes ready.
        buffer.Advance(Timestamp(EventTime(3) + 100'000));
        EXPECT_EQ(buffer.watermark(), Timestamp(400));

        const std::vector<int> released = buffer.PopReady();

        ASSERT_EQ(released.size(), 4u);
        if (reference.empty()) {
            reference = released;
        }
        EXPECT_EQ(released, reference) << "permutation " << permutations;
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));

    EXPECT_EQ(permutations, 24u);
    EXPECT_EQ(reference, (std::vector<int>{0, 1, 2, 3}));
}

TEST(TimestampBufferTest, WatermarkIsMonotone) {
    TimestampBuffer<int> buffer(BufferOptions{1'000, 50'000});
    buffer.Push(1, Timestamp(5'000), "cam-a", Timestamp(5'000));
    buffer.Push(2, Timestamp(9'000), "cam-b", Timestamp(9'000));

    const Timestamp first = buffer.watermark();
    EXPECT_EQ(first, Timestamp(4'000));

    buffer.Push(3, Timestamp(12'000), "cam-a", Timestamp(12'000));
    EXPECT_EQ(buffer.watermark(), Timestamp(8'000));

    buffer.Push(4, Timestamp(20'000), "cam-b", Timestamp(20'000));
    EXPECT_GE(buffer.watermark(), Timestamp(8'000));
    EXPECT_EQ(buffer.watermark(), Timestamp(11'000));
}

TEST(TimestampBufferTest, SilentCameraDoesNotStallTheWatermark) {
    TimestampBuffer<int> buffer(BufferOptions{1'000, 2'000});
    buffer.Push(1, Timestamp(1'000), "cam-a", Timestamp(1'000));
    buffer.Push(2, Timestamp(5'000), "cam-b", Timestamp(2'000));

    // cam-a is still active: the watermark is held back by its newest event.
    EXPECT_EQ(buffer.watermark(), Timestamp{});

    // cam-a has been silent for longer than the timeout, so only cam-b counts.
    buffer.Push(3, Timestamp(9'000), "cam-b", Timestamp(8'000));
    EXPECT_EQ(buffer.watermark(), Timestamp(8'000));

    // Everything is silent: the watermark jumps to the newest event seen.
    buffer.Advance(Timestamp(100'000));
    EXPECT_EQ(buffer.watermark(), Timestamp(9'000));
}

TEST(TimestampBufferTest, DrainAllKeepsDeterministicOrder) {
    TimestampBuffer<int> buffer(BufferOptions{0, 10'000});
    buffer.Push(3, Timestamp(300), "cam-b", Timestamp(300));
    buffer.Push(1, Timestamp(100), "cam-a", Timestamp(100));
    buffer.Push(2, Timestamp(200), "cam-a", Timestamp(200));

    const std::vector<int> drained = buffer.DrainAll();
    EXPECT_EQ(drained, (std::vector<int>{1, 2, 3}));
    EXPECT_TRUE(buffer.empty());
}

TEST(WindowSchedulerTest, EmitsOverlappingWindowsAfterWarmup) {
    WindowScheduler scheduler(WindowOptions{1'000, 200, 500, 300});
    scheduler.Start(Timestamp(1'000));
    ASSERT_TRUE(scheduler.started());

    EXPECT_TRUE(scheduler.Advance(Timestamp(1'500)).empty());

    std::vector<Window> windows = scheduler.Advance(Timestamp(2'500));
    ASSERT_EQ(windows.size(), 1u);
    EXPECT_EQ(windows[0], (Window{Timestamp(1'000), Timestamp(2'000), 0}));

    windows = scheduler.Advance(Timestamp(4'000));
    ASSERT_EQ(windows.size(), 2u);
    EXPECT_EQ(windows[0], (Window{Timestamp(1'800), Timestamp(2'800), 1}));
    EXPECT_EQ(windows[1], (Window{Timestamp(2'600), Timestamp(3'600), 2}));
}

TEST(WindowSchedulerTest, CommitLagFreezesWindows) {
    WindowScheduler scheduler(WindowOptions{1'000, 200, 500, 300});
    scheduler.Start(Timestamp(1'000));
    (void)scheduler.Advance(Timestamp(4'000));

    EXPECT_EQ(scheduler.StateFor(Timestamp(1'500)), WindowState::Emitted);
    EXPECT_EQ(scheduler.StateFor(Timestamp(3'000)), WindowState::Emitted);
    EXPECT_EQ(scheduler.StateFor(Timestamp(9'000)), WindowState::Pending);

    const auto reopen = scheduler.ReopenFor(Timestamp(1'500));
    ASSERT_TRUE(reopen.has_value());
    EXPECT_EQ(reopen->index, 0u);

    std::vector<Window> commit = scheduler.AdvanceCommit(Timestamp(4'000));
    ASSERT_EQ(commit.size(), 3u);
    EXPECT_EQ(commit[0].index, 0u);
    EXPECT_EQ(commit[2].index, 2u);

    // Nothing is frozen until the caller marks the windows committed.
    EXPECT_EQ(scheduler.AdvanceCommit(Timestamp(4'000)).size(), 3u);

    scheduler.MarkCommitted(0);
    EXPECT_TRUE(scheduler.IsCommitted(0));
    EXPECT_EQ(scheduler.StateFor(Timestamp(1'500)), WindowState::Committed);
    EXPECT_FALSE(scheduler.ReopenFor(Timestamp(1'500)).has_value());

    const std::vector<Window> remaining = scheduler.AdvanceCommit(Timestamp(4'000));
    ASSERT_EQ(remaining.size(), 2u);
    EXPECT_EQ(remaining[0].index, 1u);
}

TEST(WindowSchedulerTest, GridIsAlignedToFirstEvent) {
    WindowScheduler scheduler(WindowOptions{1'000, 200, 0, 0});
    scheduler.Start(Timestamp(5'500));

    const Window window = scheduler.WindowFor(0);
    EXPECT_EQ(window.begin, Timestamp(5'000));
    EXPECT_EQ(window.end, Timestamp(6'000));
}

TEST(ReplayGuardTest, ClassifiesArrivalsAndSuppressesDuplicates) {
    ReplayGuard guard;

    EXPECT_EQ(guard.Classify(1, Timestamp(500), Timestamp(1'000), true, WindowState::Pending),
              ArrivalClass::InOrder);
    EXPECT_EQ(guard.Classify(2, Timestamp(500), Timestamp(1'000), true, WindowState::Emitted),
              ArrivalClass::LateWithinWindow);
    EXPECT_EQ(guard.Classify(3, Timestamp(500), Timestamp(1'000), true, WindowState::Committed),
              ArrivalClass::TooLate);
    EXPECT_EQ(guard.Classify(1, Timestamp(500), Timestamp(1'000), true, WindowState::Pending),
              ArrivalClass::Duplicate);
    EXPECT_EQ(guard.Classify(4, Timestamp(2'000), Timestamp(1'000), true, WindowState::Emitted),
              ArrivalClass::InOrder);
    EXPECT_EQ(guard.Classify(5, Timestamp(500), Timestamp{}, false, WindowState::Committed),
              ArrivalClass::InOrder);

    EXPECT_EQ(guard.stats().in_order, 3u);
    EXPECT_EQ(guard.stats().late, 1u);
    EXPECT_EQ(guard.stats().too_late, 1u);
    EXPECT_EQ(guard.stats().duplicates, 1u);
}

TEST(ReplayGuardTest, DuplicateCacheIsBounded) {
    ReplayGuard guard(ReplayGuardOptions{2});
    EXPECT_EQ(guard.Classify(1, Timestamp(10), Timestamp{}, false, WindowState::Pending), ArrivalClass::InOrder);
    EXPECT_EQ(guard.Classify(2, Timestamp(10), Timestamp{}, false, WindowState::Pending), ArrivalClass::InOrder);
    EXPECT_EQ(guard.Classify(1, Timestamp(10), Timestamp{}, false, WindowState::Pending), ArrivalClass::Duplicate);

    EXPECT_EQ(guard.Classify(3, Timestamp(10), Timestamp{}, false, WindowState::Pending), ArrivalClass::InOrder);
    // The oldest id was evicted, so it is accepted again.
    EXPECT_EQ(guard.Classify(1, Timestamp(10), Timestamp{}, false, WindowState::Pending), ArrivalClass::InOrder);
}

TEST(ClockSyncEstimatorTest, NeedsMinSamplesBeforeItSyncs) {
    ClockSyncOptions options;
    options.min_samples = 5;
    options.window_samples = 10;
    options.assumed_transport_ms = 0;
    ClockSyncEstimator estimator(options);

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(estimator.AddSample(Timestamp(1'000 + i * 1'000), Timestamp(2'000 + i * 1'000)).ok());
    }
    EXPECT_FALSE(estimator.synced());
    EXPECT_NEAR(estimator.Correction(Timestamp(5'000)).offset_ms, 1'000.0, 1e-6);

    ASSERT_TRUE(estimator.AddSample(Timestamp(5'000), Timestamp(6'000)).ok());
    EXPECT_TRUE(estimator.synced());
    EXPECT_NEAR(estimator.Stats().offset_ms, 1'000.0, 1e-6);
    EXPECT_NEAR(estimator.Stats().drift_ppm, 0.0, 1e-6);
}

TEST(ClockSyncEstimatorTest, EstimatesConstantOffset) {
    ClockSyncOptions options;
    options.min_samples = 1;
    options.window_samples = 200;
    options.assumed_transport_ms = 0;
    ClockSyncEstimator estimator(options);

    for (int i = 0; i < 100; ++i) {
        const Timestamp edge(1'700'000'000'000LL + i * 1'000);
        ASSERT_TRUE(estimator.AddSample(edge, edge + Timestamp(1'500)).ok());
    }

    const ClockSyncStats stats = estimator.Stats();
    EXPECT_TRUE(stats.synced);
    EXPECT_NEAR(stats.offset_ms, 1'500.0, 1e-6);
    EXPECT_NEAR(stats.drift_ppm, 0.0, 1e-6);
    EXPECT_NEAR(stats.residual_ms, 0.0, 1e-6);

    const ClockCorrection correction = estimator.Correction(Timestamp(1'700'000'050'000LL));
    EXPECT_EQ(correction.Apply(Timestamp(1'700'000'050'000LL)), Timestamp(1'700'000'051'500LL));
}

TEST(ClockSyncEstimatorTest, EstimatesDriftAndRejectsSpikes) {
    ClockSyncOptions options;
    options.min_samples = 1;
    options.window_samples = 200;
    options.assumed_transport_ms = 0;
    options.outlier_sigma = 3.0;
    ClockSyncEstimator estimator(options);

    const Timestamp origin(1'700'000'000'000LL);
    for (int i = 0; i < 40; ++i) {
        const Timestamp edge(origin.count() + i * 10'000);
        const double drift = 100e-6 * static_cast<double>(i * 10'000);
        const double noise = (i % 2 == 0) ? 0.0 : 1.0;
        ASSERT_TRUE(
            estimator.AddSample(edge, edge + Timestamp(static_cast<std::int64_t>(std::llround(500.0 + drift + noise))))
                .ok());
    }
    for (int i = 0; i < 2; ++i) {
        const Timestamp edge(origin.count() + (i + 1) * 10'000);
        ASSERT_TRUE(estimator.AddSample(edge, edge + Timestamp(20'000)).ok());
    }

    const ClockSyncStats stats = estimator.Stats();
    EXPECT_TRUE(stats.synced);
    EXPECT_EQ(stats.samples, 42u);
    EXPECT_LT(stats.accepted, stats.samples);
    EXPECT_NEAR(stats.drift_ppm, 100.0, 0.5);
    EXPECT_NEAR(stats.offset_ms, 500.0 + 100e-6 * 390'000.0, 5.0);
}

TEST(ClockSyncEstimatorTest, ClampsOffsetAndDrift) {
    ClockSyncOptions options;
    options.min_samples = 1;
    options.window_samples = 10;
    options.assumed_transport_ms = 0;
    options.max_offset_ms = 500;
    options.max_drift_ppm = 10.0;
    ClockSyncEstimator estimator(options);

    const Timestamp origin(1'000);
    for (int i = 0; i < 10; ++i) {
        const Timestamp edge(origin.count() + i * 1'000);
        const Timestamp master(edge.count() + 5'000 + i * 100);
        ASSERT_TRUE(estimator.AddSample(edge, master).ok());
    }

    const ClockSyncStats stats = estimator.Stats();
    EXPECT_NEAR(stats.offset_ms, 500.0, 1e-6);
    EXPECT_LE(std::abs(stats.drift_ppm), 10.0 + 1e-6);
}

TEST(ClockCorrectionTest, ApplyAndRevertRoundTrip) {
    ClockCorrection correction;
    correction.reference = Timestamp(100'000);
    correction.offset_ms = 250.0;
    correction.drift_ppm = 100.0;

    for (const std::int64_t edge : {0LL, 50'000LL, 100'000LL, 250'000LL}) {
        const Timestamp corrected = correction.Apply(Timestamp(edge));
        EXPECT_NEAR(static_cast<double>((correction.Revert(corrected) - Timestamp(edge)).count()), 0.0, 2.0);
    }
}

TEST(ClockCorrectionTest, SynchronizeKeepsRawTiming) {
    Tracklet tracklet;
    tracklet.begin = Timestamp(10'000);
    tracklet.end = Timestamp(10'500);
    Observation first;
    first.ts = Timestamp(10'000);
    Observation second;
    second.ts = Timestamp(10'500);
    tracklet.observations = {first, second};

    ClockCorrection correction;
    correction.reference = Timestamp(10'000);
    correction.offset_ms = 1'000.0;

    const SyncedTracklet synced = Synchronize(std::move(tracklet), correction, Timestamp(12'000));
    EXPECT_EQ(synced.raw_begin, Timestamp(10'000));
    EXPECT_EQ(synced.raw_end, Timestamp(10'500));
    EXPECT_EQ(synced.received_at, Timestamp(12'000));
    EXPECT_EQ(synced.tracklet.begin, Timestamp(11'000));
    EXPECT_EQ(synced.tracklet.end, Timestamp(11'500));
    ASSERT_EQ(synced.tracklet.observations.size(), 2u);
    EXPECT_EQ(synced.tracklet.observations[0].ts, Timestamp(11'000));
    EXPECT_EQ(synced.tracklet.observations[1].ts, Timestamp(11'500));
}

TEST(MetricsCollectorTest, ComputesPercentilesAndCounters) {
    MetricsCollector collector(1'000);
    for (int i = 1; i <= 100; ++i) {
        collector.RecordIngestLatency(Timestamp(i));
    }
    collector.RecordSolveTime(Timestamp(7));
    collector.counters().accepted = 3;

    const PipelineStats stats = collector.Snapshot();
    EXPECT_EQ(stats.ingest_latency_ms.count, 100u);
    EXPECT_NEAR(stats.ingest_latency_ms.mean_ms, 50.5, 1e-9);
    EXPECT_NEAR(stats.ingest_latency_ms.p50_ms, 50.5, 1e-9);
    EXPECT_NEAR(stats.ingest_latency_ms.p95_ms, 95.05, 1e-9);
    EXPECT_DOUBLE_EQ(stats.ingest_latency_ms.max_ms, 100.0);
    EXPECT_EQ(stats.solve_ms.count, 1u);
    EXPECT_EQ(stats.counters.accepted, 3u);

    collector.Reset();
    EXPECT_EQ(collector.Snapshot().counters.accepted, 0u);
    EXPECT_EQ(collector.Snapshot().ingest_latency_ms.count, 0u);
}

TEST(MetricsCollectorTest, RingKeepsOnlyTheNewestValues) {
    MetricsCollector collector(3);
    collector.RecordLateness(Timestamp(1));
    collector.RecordLateness(Timestamp(2));
    collector.RecordLateness(Timestamp(3));
    collector.RecordLateness(Timestamp(4));

    const LatencyStats stats = collector.Snapshot().lateness_ms;
    EXPECT_EQ(stats.count, 3u);
    EXPECT_DOUBLE_EQ(stats.max_ms, 4.0);
    EXPECT_NEAR(stats.mean_ms, 3.0, 1e-9);
}

} // namespace tmcmt::pipeline
