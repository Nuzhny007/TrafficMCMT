#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/IResultSink.h"
#include "trafficmcmt/io/ITrackletSource.h"
#include "trafficmcmt/pipeline/MasterPipeline.h"

#include "TrackletTestHelpers.h"

namespace tmcmt::pipeline {
namespace {

using Item = std::pair<Tracklet, Timestamp>;

class FakeSource : public ITrackletSource {
public:
    explicit FakeSource(std::vector<Item> items) : m_items(std::move(items)) {}

    Result<bool> Next(Tracklet& out) override {
        Timestamp ignored{};
        return Next(out, ignored);
    }

    Result<bool> Next(Tracklet& out, Timestamp& received_at) override {
        if (m_pos >= m_items.size()) {
            return false;
        }
        out = m_items[m_pos].first;
        received_at = m_items[m_pos].second;
        ++m_pos;
        return true;
    }

    Status Reset() override {
        m_pos = 0;
        return Status::Ok();
    }

    void Stop() noexcept override {
        stopped = true;
    }

    std::string Describe() const override {
        return "fake-source";
    }

    bool stopped = false;

private:
    std::vector<Item> m_items;
    std::size_t m_pos = 0;
};

class CapturingSink : public IResultSink {
public:
    Status Publish(const GlobalTrackUpdate& update) override {
        updates.push_back(update);
        return Status::Ok();
    }

    Status PublishMerge(const GlobalIdMergeEvent& event) override {
        merges.push_back(event);
        return Status::Ok();
    }

    Status Flush() override {
        ++flushes;
        return Status::Ok();
    }

    std::string Describe() const override {
        return "capturing-sink";
    }

    std::vector<GlobalTrackUpdate> updates;
    std::vector<GlobalIdMergeEvent> merges;
    int flushes = 0;
};

MasterConfig BaseConfig() {
    MasterConfig config;
    config.mode = PipelineMode::Online;
    config.window_ms = 10'000;
    config.window_overlap_ms = 2'000;
    config.commit_lag_ms = 2'000;
    config.warmup_ms = 1'000;
    config.retention_ms = 120'000;
    config.ingest.max_lateness_ms = 2'000;
    config.watermark.silent_timeout_ms = 60'000;
    config.reid.embedding_dim = 4;
    config.reid.top_k = 10;
    config.clock_sync.enabled = false;
    config.online.clock = OnlineClockKind::Event;
    return config;
}

Tracklet MakeSegment(const std::string& camera, std::uint64_t local_id, std::int64_t begin_ms, std::int64_t end_ms,
                     std::vector<float> embedding) {
    Tracklet tracklet = test::MakeTracklet("session-7", camera, local_id, 0, begin_ms);
    tracklet.begin = Timestamp(begin_ms);
    tracklet.end = Timestamp(end_ms);
    tracklet.observations[0].ts = tracklet.begin;
    tracklet.observations[1].ts = tracklet.end;
    tracklet.observations[0].world = WorldPoint{10.f, 10.f, true};
    tracklet.observations[1].world = WorldPoint{10.f, 10.f, true};
    tracklet.embedding.dim = static_cast<std::uint16_t>(embedding.size());
    tracklet.embedding.values = std::move(embedding);
    AssignTrackletId(tracklet);
    return tracklet;
}

/// Two identities, three cameras, well-separated in time.
std::vector<Item> ThreeCameraData() {
    const std::vector<float> a{1.f, 0.f, 0.f, 0.f};
    const std::vector<float> b{-1.f, 0.f, 0.f, 0.f};
    std::vector<Item> items;
    items.emplace_back(MakeSegment("cam-01", 1, 1'000, 2'000, a), Timestamp(2'000));
    items.emplace_back(MakeSegment("cam-01", 2, 3'000, 4'000, b), Timestamp(4'000));
    items.emplace_back(MakeSegment("cam-02", 1, 6'000, 7'000, a), Timestamp(7'000));
    items.emplace_back(MakeSegment("cam-02", 2, 8'000, 9'000, b), Timestamp(9'000));
    items.emplace_back(MakeSegment("cam-03", 1, 11'000, 12'000, a), Timestamp(12'000));
    items.emplace_back(MakeSegment("cam-03", 2, 13'000, 14'000, b), Timestamp(14'000));
    return items;
}

/// Rewrites the receive time to simulate an edge clock offset.
void ApplyClockOffset(std::vector<Item>& items, const std::map<std::string, std::int64_t>& offsets) {
    for (Item& item : items) {
        const auto it = offsets.find(item.first.camera);
        if (it == offsets.end()) {
            continue;
        }
        item.second = item.first.end + Timestamp(it->second);
    }
}

void Build(const MasterConfig& config, std::vector<Item> items, std::unique_ptr<MasterPipeline>& pipeline,
           CapturingSink*& sink, FakeSource*& source) {
    auto ownedSource = std::make_unique<FakeSource>(std::move(items));
    auto ownedSink = std::make_unique<CapturingSink>();
    source = ownedSource.get();
    sink = ownedSink.get();

    PipelineDependencies dependencies;
    dependencies.source = std::move(ownedSource);
    dependencies.sinks.push_back(std::move(ownedSink));

    auto created = MasterPipeline::Create(config, std::move(dependencies));
    ASSERT_TRUE(created.has_value()) << created.status().ToString();
    pipeline = std::move(created).value();
}

std::set<std::set<TrackletId>> FinalGrouping(const std::vector<GlobalTrackUpdate>& updates) {
    std::map<GlobalId, GlobalTrackUpdate> latest;
    for (const GlobalTrackUpdate& update : updates) {
        latest[update.global_id] = update;
    }

    std::set<std::set<TrackletId>> groups;
    for (const auto& [id, update] : latest) {
        (void)id;
        if (!update.is_final || update.segments.empty()) {
            continue;
        }
        std::set<TrackletId> group;
        for (const Tracklet& segment : update.segments) {
            group.insert(segment.id);
        }
        groups.insert(std::move(group));
    }
    return groups;
}

std::string Serialize(const std::vector<GlobalTrackUpdate>& updates, const std::vector<GlobalIdMergeEvent>& merges) {
    std::ostringstream out;
    for (const GlobalTrackUpdate& update : updates) {
        out << update.global_id << '|' << update.is_final << '|' << update.updated_at.count();
        for (const Tracklet& segment : update.segments) {
            out << '|' << segment.id << ':' << segment.begin.count() << '-' << segment.end.count();
        }
        out << '\n';
    }
    for (const GlobalIdMergeEvent& merge : merges) {
        out << "merge|" << merge.old_id << '|' << merge.new_id << '|' << merge.ts.count() << '\n';
    }
    return out.str();
}

TEST(MasterPipelineTest, OfflineGroupsTheTwoIdentities) {
    std::unique_ptr<MasterPipeline> pipeline;
    CapturingSink* sink = nullptr;
    FakeSource* source = nullptr;
    Build(BaseConfig(), ThreeCameraData(), pipeline, sink, source);
    ASSERT_NE(pipeline, nullptr);

    const Status status = pipeline->RunOffline();
    ASSERT_TRUE(status.ok()) << status.ToString();

    const std::set<std::set<TrackletId>> groups = FinalGrouping(sink->updates);
    ASSERT_EQ(groups.size(), 2u);
    for (const std::set<TrackletId>& group : groups) {
        EXPECT_EQ(group.size(), 3u);
    }
    EXPECT_EQ(sink->flushes, 1);
}

TEST(MasterPipelineTest, OnlineMatchesTheOfflineGrouping) {
    std::unique_ptr<MasterPipeline> offline;
    CapturingSink* offlineSink = nullptr;
    FakeSource* offlineSource = nullptr;
    Build(BaseConfig(), ThreeCameraData(), offline, offlineSink, offlineSource);
    ASSERT_TRUE(offline->RunOffline().ok());

    std::unique_ptr<MasterPipeline> online;
    CapturingSink* onlineSink = nullptr;
    FakeSource* onlineSource = nullptr;
    Build(BaseConfig(), ThreeCameraData(), online, onlineSink, onlineSource);
    const Status status = online->RunOnline();
    ASSERT_TRUE(status.ok()) << status.ToString();

    EXPECT_EQ(FinalGrouping(onlineSink->updates), FinalGrouping(offlineSink->updates));

    const PipelineStats stats = online->Stats();
    EXPECT_EQ(stats.counters.ingested, 6u);
    EXPECT_EQ(stats.counters.accepted, 6u);
    EXPECT_EQ(stats.counters.too_late, 0u);
    EXPECT_GT(stats.counters.windows_solved, 0u);
    EXPECT_GT(stats.counters.windows_committed, 0u);
    EXPECT_GT(stats.counters.results_published, 0u);
}

TEST(MasterPipelineTest, OnlineToleratesCameraClockOffsets) {
    std::unique_ptr<MasterPipeline> offline;
    CapturingSink* offlineSink = nullptr;
    FakeSource* offlineSource = nullptr;
    Build(BaseConfig(), ThreeCameraData(), offline, offlineSink, offlineSource);
    ASSERT_TRUE(offline->RunOffline().ok());

    MasterConfig config = BaseConfig();
    config.clock_sync.enabled = true;
    config.clock_sync.min_samples = 1;
    config.clock_sync.assumed_transport_ms = 0;

    std::vector<Item> items = ThreeCameraData();
    ApplyClockOffset(items, {{"cam-01", 0}, {"cam-02", 1'500}, {"cam-03", -2'000}});

    std::unique_ptr<MasterPipeline> online;
    CapturingSink* onlineSink = nullptr;
    FakeSource* onlineSource = nullptr;
    Build(config, std::move(items), online, onlineSink, onlineSource);
    const Status status = online->RunOnline();
    ASSERT_TRUE(status.ok()) << status.ToString();

    EXPECT_EQ(FinalGrouping(onlineSink->updates), FinalGrouping(offlineSink->updates));

    const PipelineStats stats = online->Stats();
    ASSERT_EQ(stats.cameras.size(), 3u);
    for (const CameraClockStats& camera : stats.cameras) {
        EXPECT_TRUE(camera.synced) << camera.camera;
    }
    EXPECT_EQ(stats.counters.too_late, 0u);
}

TEST(MasterPipelineTest, ReplayIsByteForByteDeterministicWithEventClock) {
    MasterConfig config = BaseConfig();
    config.clock_sync.enabled = true;
    config.clock_sync.min_samples = 1;
    config.clock_sync.assumed_transport_ms = 0;

    std::vector<Item> items = ThreeCameraData();
    ApplyClockOffset(items, {{"cam-01", 0}, {"cam-02", 1'500}, {"cam-03", -2'000}});

    std::string first;
    std::string second;
    for (std::string* output : {&first, &second}) {
        std::unique_ptr<MasterPipeline> pipeline;
        CapturingSink* sink = nullptr;
        FakeSource* source = nullptr;
        Build(config, items, pipeline, sink, source);
        ASSERT_TRUE(pipeline->RunOnline().ok());
        *output = Serialize(sink->updates, sink->merges);
    }

    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST(MasterPipelineTest, LateTrackletReopensTheWindowOnce) {
    MasterConfig config = BaseConfig();
    config.watermark.silent_timeout_ms = 60'000;

    std::vector<Item> items;
    for (std::int64_t t = 0; t <= 16'000; t += 1'000) {
        items.emplace_back(MakeSegment("cam-01", static_cast<std::uint64_t>(t / 1'000 + 1), t, t + 100,
                                       {1.f, 0.f, 0.f, 0.f}),
                           Timestamp(t));
        if (t == 13'000) {
            // Window [0, 10 000) is solved but not committed yet.
            items.emplace_back(MakeSegment("cam-01", 500, 4'900, 5'000, {-1.f, 0.f, 0.f, 0.f}), Timestamp(t));
        }
        if (t == 16'000) {
            // Window [0, 10 000) is committed by now.
            items.emplace_back(MakeSegment("cam-01", 501, 5'900, 6'000, {-1.f, 0.f, 0.f, 0.f}), Timestamp(t));
        }
    }

    std::unique_ptr<MasterPipeline> pipeline;
    CapturingSink* sink = nullptr;
    FakeSource* source = nullptr;
    Build(config, std::move(items), pipeline, sink, source);
    const Status status = pipeline->RunOnline();
    ASSERT_TRUE(status.ok()) << status.ToString();

    const PipelineStats stats = pipeline->Stats();
    EXPECT_EQ(stats.counters.windows_reopened, 1u);
    EXPECT_EQ(stats.counters.late, 1u);
    EXPECT_EQ(stats.counters.too_late, 1u);
    EXPECT_EQ(stats.counters.dropped, 1u);

    bool sawLate = false;
    bool sawTooLate = false;
    for (const GlobalTrackUpdate& update : sink->updates) {
        for (const Tracklet& segment : update.segments) {
            if (segment.local_track_id == 500) {
                sawLate = true;
            }
            if (segment.local_track_id == 501) {
                sawTooLate = true;
            }
        }
    }
    EXPECT_TRUE(sawLate);
    EXPECT_FALSE(sawTooLate);
}

TEST(MasterPipelineTest, SilentCameraDoesNotStallTheWatermark) {
    MasterConfig config = BaseConfig();
    config.watermark.silent_timeout_ms = 3'000;

    std::vector<Item> items;
    for (std::int64_t t = 0; t <= 40'000; t += 1'000) {
        if (t <= 5'000) {
            items.emplace_back(MakeSegment("cam-a", static_cast<std::uint64_t>(t / 1'000 + 1), t, t + 100,
                                           {1.f, 0.f, 0.f, 0.f}),
                               Timestamp(t));
        }
        items.emplace_back(MakeSegment("cam-b", static_cast<std::uint64_t>(t / 1'000 + 1), t, t + 100,
                                       {1.f, 0.f, 0.f, 0.f}),
                           Timestamp(t));
        items.emplace_back(MakeSegment("cam-c", static_cast<std::uint64_t>(t / 1'000 + 1), t, t + 100,
                                       {1.f, 0.f, 0.f, 0.f}),
                           Timestamp(t));
    }

    std::unique_ptr<MasterPipeline> pipeline;
    CapturingSink* sink = nullptr;
    FakeSource* source = nullptr;
    Build(config, std::move(items), pipeline, sink, source);
    const Status status = pipeline->RunOnline();
    ASSERT_TRUE(status.ok()) << status.ToString();

    const PipelineStats stats = pipeline->Stats();
    EXPECT_GE(stats.counters.watermark_ms, 30'000);
    EXPECT_GT(stats.counters.windows_solved, 0u);
    EXPECT_GT(stats.counters.windows_committed, 0u);
}

TEST(MasterPipelineTest, RespectsMaxEventsAndStopsTheSource) {
    MasterConfig config = BaseConfig();
    config.online.max_events = 3;

    std::unique_ptr<MasterPipeline> pipeline;
    CapturingSink* sink = nullptr;
    FakeSource* source = nullptr;
    Build(config, ThreeCameraData(), pipeline, sink, source);
    ASSERT_TRUE(pipeline->RunOnline().ok());

    EXPECT_EQ(pipeline->Stats().counters.ingested, 3u);

    pipeline->Stop();
    EXPECT_TRUE(source->stopped);
    EXPECT_EQ(sink->flushes, 1);
}

TEST(MasterPipelineTest, RawTimingIsRecoverable) {
    MasterConfig config = BaseConfig();
    config.clock_sync.enabled = true;
    config.clock_sync.min_samples = 1;
    config.clock_sync.assumed_transport_ms = 0;

    std::vector<Item> items = ThreeCameraData();
    ApplyClockOffset(items, {{"cam-01", 0}, {"cam-02", 1'500}, {"cam-03", -2'000}});

    std::unique_ptr<MasterPipeline> pipeline;
    CapturingSink* sink = nullptr;
    FakeSource* source = nullptr;
    Build(config, items, pipeline, sink, source);
    ASSERT_TRUE(pipeline->RunOnline().ok());

    for (const Item& item : items) {
        const auto timing = pipeline->RawTimingFor(item.first.id);
        ASSERT_TRUE(timing.has_value());
        EXPECT_EQ(timing->raw_begin, item.first.begin);
        EXPECT_EQ(timing->raw_end, item.first.end);
        EXPECT_EQ(timing->received_at, item.second);
        const Timestamp corrected = timing->correction.Apply(timing->raw_end);
        EXPECT_NEAR(static_cast<double>((timing->correction.Revert(corrected) - timing->raw_end).count()), 0.0, 2.0);
    }
}

} // namespace

} // namespace tmcmt::pipeline
