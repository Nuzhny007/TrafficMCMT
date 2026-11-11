#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "trafficmcmt/core/Clock.h"
#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/flow/FlowAssociator.h"
#include "trafficmcmt/io/IResultSink.h"
#include "trafficmcmt/io/ITrackletSource.h"
#include "trafficmcmt/pipeline/ClockSyncEstimator.h"
#include "trafficmcmt/pipeline/MetricsCollector.h"
#include "trafficmcmt/pipeline/ReplayGuard.h"
#include "trafficmcmt/pipeline/TimestampBuffer.h"
#include "trafficmcmt/pipeline/WindowScheduler.h"

namespace tmcmt::flow {
class GlobalIdManager;
}

namespace tmcmt::reid {
class GalleryManager;
}

namespace tmcmt::st {
class CameraLinkModel;
}

namespace tmcmt::pipeline {

/// Injectable time source. `event` mode is implemented by the pipeline itself
/// and ignores `wall`.
struct PipelineClock {
    std::function<Timestamp()> wall = NowMs;
    std::function<Timestamp()> steady = SteadyNowMs;
};

/// Everything the pipeline needs from the outside world. Sources and sinks are
/// injected so that this module never links protobuf, OpenCV or ZeroMQ.
struct PipelineDependencies {
    std::unique_ptr<ITrackletSource> source;
    std::vector<std::unique_ptr<IResultSink>> sinks;
    std::shared_ptr<spdlog::logger> logger;
    PipelineClock clock;
};

/// Raw edge-domain timing kept next to the corrected tracklet.
struct RawTiming {
    CameraId camera;
    Timestamp raw_begin{};
    Timestamp raw_end{};
    ClockCorrection correction;
    Timestamp received_at{};
};

/// Master orchestration: offline baseline and online windowed association.
///
/// Offline drains the source, associates everything and emits one final update
/// per identity. Online applies clock corrections, buffers by event time,
/// solves overlapping windows and emits incremental updates plus merge events.
class MasterPipeline {
public:
    [[nodiscard]] static Result<std::unique_ptr<MasterPipeline>> Create(const MasterConfig& config,
                                                                        PipelineDependencies dependencies);
    ~MasterPipeline();

    MasterPipeline(const MasterPipeline&) = delete;
    MasterPipeline& operator=(const MasterPipeline&) = delete;

    [[nodiscard]] Status Start();

    [[nodiscard]] Status RunOffline();
    [[nodiscard]] Status RunOnline();

    /// Thread-safe. Sets the stop flag and unblocks the source.
    void Stop() noexcept;

    [[nodiscard]] PipelineStats Stats();

    /// Raw timestamps and the applied correction for an ingested tracklet.
    [[nodiscard]] std::optional<RawTiming> RawTimingFor(TrackletId id) const;

    /// Every retained raw-timing entry, sorted by tracklet id.
    [[nodiscard]] std::vector<std::pair<TrackletId, RawTiming>> RawTimings() const;

    /// `(tracklet id, global id)` pairs sorted by tracklet id.
    [[nodiscard]] std::vector<std::pair<TrackletId, GlobalId>> Assignments() const;

private:
    MasterPipeline(const MasterConfig& config, PipelineDependencies dependencies);

    [[nodiscard]] Status Initialize();

    [[nodiscard]] Status Ingest(SyncedTracklet synced, ArrivalClass arrival, Timestamp now_wall);
    [[nodiscard]] Status ProcessReady();
    [[nodiscard]] Status Solve(const Window& window);
    [[nodiscard]] Status CommitClosedWindows(Timestamp watermark);
    [[nodiscard]] Status SolveWindow(const Window& window, std::vector<GlobalId>& touched);
    [[nodiscard]] Status Flush();

    [[nodiscard]] Status EmitUpdates(const std::vector<GlobalId>& ids, Timestamp now, bool finalize);
    [[nodiscard]] Status EmitUpdate(GlobalId id, Timestamp now, bool finalize);
    [[nodiscard]] Status EmitNewMerges();
    [[nodiscard]] Status Publish(const GlobalTrackUpdate& update);
    [[nodiscard]] Status PublishMerge(const GlobalIdMergeEvent& event);
    [[nodiscard]] Status FlushSinks();

    [[nodiscard]] std::vector<Tracklet> WindowTracklets(const Window& window) const;
    [[nodiscard]] std::vector<flow::TransitionEdge> BuildTransitions(const std::vector<Tracklet>& tracklets) const;
    void AddToStore(SyncedTracklet synced);
    void PruneRetention(Timestamp watermark);
    void UpdateCameraStats();
    [[nodiscard]] Timestamp NowWall() const;
    [[nodiscard]] bool IdleExpired(Timestamp now, Timestamp last_data) const;

    MasterConfig m_config;
    PipelineDependencies m_dependencies;
    std::shared_ptr<spdlog::logger> m_logger;
    PipelineClock m_clock;
    std::atomic<bool> m_stop{false};

    std::unique_ptr<reid::GalleryManager> m_gallery;
    std::unique_ptr<st::CameraLinkModel> m_linkModel;
    std::unique_ptr<flow::GlobalIdManager> m_ids;

    TimestampBuffer<SyncedTracklet> m_buffer;
    WindowScheduler m_scheduler;
    ReplayGuard m_guard;
    MetricsCollector m_metrics;
    std::unordered_map<CameraId, ClockSyncEstimator> m_clockSync;

    std::unordered_map<TrackletId, Tracklet> m_store;
    std::unordered_map<TrackletId, RawTiming> m_rawTiming;

    std::size_t m_emittedMerges = 0;
    Timestamp m_max_event{};
    bool m_started = false;
};

} // namespace tmcmt::pipeline
