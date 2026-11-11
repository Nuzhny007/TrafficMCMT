#include "trafficmcmt/pipeline/MasterPipeline.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>

#include "trafficmcmt/flow/GlobalIdManager.h"
#include "trafficmcmt/reid/GalleryManager.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/PairCandidateGenerator.h"

namespace tmcmt::pipeline {
namespace {

bool HasUsableEmbedding(const Tracklet& tracklet, int dim) {
    return tracklet.embedding.IsValid() && tracklet.embedding.dim == dim;
}

void SortTracklets(std::vector<Tracklet>& tracklets) {
    std::sort(tracklets.begin(), tracklets.end(), [](const Tracklet& lhs, const Tracklet& rhs) {
        if (lhs.begin != rhs.begin) {
            return lhs.begin < rhs.begin;
        }
        return lhs.id < rhs.id;
    });
}

ObjectType DominantType(const std::vector<Tracklet>& segments) {
    std::array<std::size_t, 6> counts{};
    for (const Tracklet& segment : segments) {
        const auto index = static_cast<std::size_t>(segment.type);
        if (index < counts.size()) {
            ++counts[index];
        }
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < counts.size(); ++i) {
        if (counts[i] > counts[best]) {
            best = i;
        }
    }
    return static_cast<ObjectType>(best);
}

flow::FlowAssociationOptions MakeFlowOptions(const MasterConfig& config) {
    flow::FlowAssociationOptions options;
    options.solver = config.flow.solver;
    options.link_reward = config.flow.link_reward;
    options.max_candidates_per_node = static_cast<std::size_t>(std::max(1, config.flow.max_candidates_per_node));
    options.max_component_nodes = static_cast<std::size_t>(std::max(1, config.flow.max_component_nodes));
    options.commit_lag = Timestamp(config.commit_lag_ms);
    return options;
}

ClockSyncOptions MakeClockSyncOptions(const ClockSyncConfig& config) {
    ClockSyncOptions options;
    options.window_samples = static_cast<std::size_t>(std::max(1, config.window_samples));
    options.min_samples = static_cast<std::size_t>(std::max(1, config.min_samples));
    options.max_offset_ms = config.max_offset_ms;
    options.max_drift_ppm = config.max_drift_ppm;
    options.assumed_transport_ms = config.assumed_transport_ms;
    options.outlier_sigma = config.outlier_sigma;
    return options;
}

} // namespace

MasterPipeline::MasterPipeline(const MasterConfig& config, PipelineDependencies dependencies)
    : m_config(config),
      m_dependencies(std::move(dependencies)),
      m_logger(m_dependencies.logger ? m_dependencies.logger : spdlog::default_logger()),
      m_clock(m_dependencies.clock),
      m_buffer(BufferOptions{config.ingest.max_lateness_ms, config.watermark.silent_timeout_ms}),
      m_scheduler(WindowOptions{config.window_ms, config.window_overlap_ms, config.warmup_ms,
                                config.commit_lag_ms}),
      m_guard(ReplayGuardOptions{}) {
    if (m_config.online.clock == OnlineClockKind::Event) {
        m_clock.wall = [this]() { return m_max_event; };
    }
}

MasterPipeline::~MasterPipeline() {
    Stop();
}

Result<std::unique_ptr<MasterPipeline>> MasterPipeline::Create(const MasterConfig& config,
                                                               PipelineDependencies dependencies) {
    return std::unique_ptr<MasterPipeline>(new MasterPipeline(config, std::move(dependencies)));
}

Status MasterPipeline::Initialize() {
    reid::GalleryOptions galleryOptions;
    galleryOptions.index.dim = m_config.reid.embedding_dim;
    galleryOptions.index.nlist = m_config.reid.nlist;
    galleryOptions.index.nprobe = m_config.reid.nprobe;
    galleryOptions.retention_ms = 0;

    auto gallery = reid::GalleryManager::Create(m_config.reid.index, galleryOptions);
    if (!gallery) {
        return gallery.status();
    }
    m_gallery = std::move(gallery).value();

    if (!m_config.st.camera_links.empty()) {
        auto model = st::CameraLinkModel::Load(m_config.st.camera_links);
        if (!model) {
            return model.status();
        }
        m_linkModel = std::make_unique<st::CameraLinkModel>(std::move(model).value());
    }

    flow::GlobalIdManagerOptions idOptions;
    idOptions.commit_lag = Timestamp(m_config.commit_lag_ms);
    m_ids = std::make_unique<flow::GlobalIdManager>(idOptions);
    return Status::Ok();
}

Status MasterPipeline::Start() {
    if (m_started) {
        return Status::Ok();
    }

    m_gallery.reset();
    m_linkModel.reset();
    m_ids.reset();

    const Status status = Initialize();
    if (!status.ok()) {
        return status;
    }

    m_stop.store(false);
    m_buffer.Reset();
    m_scheduler.Reset();
    m_guard.Reset();
    m_metrics.Reset();
    m_store.clear();
    m_rawTiming.clear();
    m_clockSync.clear();
    m_emittedMerges = 0;
    m_max_event = Timestamp{};
    m_started = true;
    return Status::Ok();
}

void MasterPipeline::Stop() noexcept {
    m_stop.store(true);
    if (m_dependencies.source) {
        m_dependencies.source->Stop();
    }
}

Timestamp MasterPipeline::NowWall() const {
    return m_clock.wall ? m_clock.wall() : NowMs();
}

bool MasterPipeline::IdleExpired(Timestamp now, Timestamp last_data) const {
    if (m_config.online.idle_timeout_ms <= 0) {
        return false;
    }
    return (now - last_data) >= Timestamp(m_config.online.idle_timeout_ms);
}

Status MasterPipeline::RunOffline() {
    if (!m_dependencies.source) {
        return Status::Error(ErrorCode::InvalidArgument, "pipeline has no tracklet source");
    }

    Status status = Initialize();
    if (!status.ok()) {
        return status;
    }

    std::vector<Tracklet> tracklets;
    for (;;) {
        Tracklet tracklet;
        auto next = m_dependencies.source->Next(tracklet);
        if (!next) {
            return next.status();
        }
        if (!next.value()) {
            break;
        }
        if (HasUsableEmbedding(tracklet, m_config.reid.embedding_dim)) {
            const Status added = m_gallery->Add(tracklet);
            if (!added.ok()) {
                m_logger->warn("offline gallery add failed for {}: {}", tracklet.id, added.ToString());
            }
        }
        tracklets.push_back(std::move(tracklet));
    }

    m_metrics.SetQueue(0, tracklets.size(), m_gallery->Size());
    if (tracklets.empty()) {
        return FlushSinks();
    }

    const std::vector<flow::TransitionEdge> transitions = BuildTransitions(tracklets);
    flow::FlowAssociator associator(MakeFlowOptions(m_config));
    auto result = associator.Associate(tracklets, transitions);
    if (!result) {
        return result.status();
    }

    std::map<GlobalId, std::vector<Tracklet>> groups;
    for (const Tracklet& tracklet : tracklets) {
        const auto it = result.value().assignments.find(tracklet.id);
        if (it == result.value().assignments.end()) {
            continue;
        }
        groups[it->second].push_back(tracklet);
    }

    for (auto& [globalId, segments] : groups) {
        Timestamp updatedAt{};
        for (const Tracklet& segment : segments) {
            updatedAt = std::max(updatedAt, segment.end);
        }

        GlobalTrackUpdate update;
        update.global_id = globalId;
        update.segments = std::move(segments);
        update.type = DominantType(update.segments);
        update.is_final = true;
        update.updated_at = updatedAt;

        status = Publish(update);
        if (!status.ok()) {
            return status;
        }
    }

    for (const GlobalIdMergeEvent& event : result.value().merge_events) {
        status = PublishMerge(event);
        if (!status.ok()) {
            return status;
        }
    }

    UpdateCameraStats();
    m_metrics.SetQueue(0, tracklets.size(), m_gallery->Size());
    return FlushSinks();
}

Status MasterPipeline::RunOnline() {
    if (!m_dependencies.source) {
        return Status::Error(ErrorCode::InvalidArgument, "pipeline has no tracklet source");
    }

    Status status = Start();
    if (!status.ok()) {
        return status;
    }

    Timestamp lastData = NowWall();
    bool reachedLimit = false;

    while (!m_stop.load() && !reachedLimit) {
        Tracklet tracklet;
        Timestamp receivedAt{};
        auto next = m_dependencies.source->Next(tracklet, receivedAt);
        if (!next) {
            if (next.status().code() == ErrorCode::Timeout) {
                const Timestamp now = NowWall();
                m_buffer.Advance(now);
                status = ProcessReady();
                if (!status.ok()) {
                    return status;
                }
                status = CommitClosedWindows(m_buffer.watermark());
                if (!status.ok()) {
                    return status;
                }
                if (IdleExpired(now, lastData)) {
                    break;
                }
                continue;
            }
            return next.status();
        }
        if (!next.value()) {
            break;
        }

        const Timestamp now = NowWall();
        lastData = now;
        ++m_metrics.counters().ingested;

        ClockCorrection correction;
        if (m_config.clock_sync.enabled) {
            auto it = m_clockSync.find(tracklet.camera);
            if (it == m_clockSync.end()) {
                it = m_clockSync
                         .emplace(tracklet.camera,
                                  ClockSyncEstimator(MakeClockSyncOptions(m_config.clock_sync)))
                         .first;
            }
            ClockSyncEstimator& estimator = it->second;
            status = estimator.AddSample(tracklet.end, receivedAt);
            if (!status.ok()) {
                return status;
            }
            correction = estimator.Correction(tracklet.end);
        }

        const Timestamp eventTime = correction.Apply(tracklet.end);
        const WindowState state = m_scheduler.StateFor(eventTime);
        const ArrivalClass arrival =
            m_guard.Classify(tracklet.id, eventTime, m_buffer.watermark(), m_buffer.has_watermark(), state);

        switch (arrival) {
        case ArrivalClass::Duplicate:
            ++m_metrics.counters().duplicates;
            continue;
        case ArrivalClass::TooLate:
            ++m_metrics.counters().too_late;
            ++m_metrics.counters().dropped;
            continue;
        case ArrivalClass::InOrder:
        case ArrivalClass::LateWithinWindow:
            break;
        }

        SyncedTracklet synced = Synchronize(std::move(tracklet), correction, receivedAt);
        status = Ingest(std::move(synced), arrival, now);
        if (!status.ok()) {
            return status;
        }

        if (m_config.online.max_events > 0 &&
            m_metrics.counters().ingested >= static_cast<std::uint64_t>(m_config.online.max_events)) {
            reachedLimit = true;
        }
    }

    return Flush();
}

Status MasterPipeline::Ingest(SyncedTracklet synced, ArrivalClass arrival, Timestamp now_wall) {
    const Timestamp eventTime = synced.tracklet.end;
    const CameraId camera = synced.tracklet.camera;
    const TrackletId id = synced.tracklet.id;

    if (!m_scheduler.started()) {
        m_scheduler.Start(eventTime);
    }
    if (eventTime > m_max_event) {
        m_max_event = eventTime;
    }

    if (m_buffer.has_watermark() && eventTime < m_buffer.watermark()) {
        m_metrics.RecordLateness(m_buffer.watermark() - eventTime);
    }
    m_metrics.RecordIngestLatency(now_wall - eventTime);

    m_buffer.Push(std::move(synced), eventTime, camera, now_wall, id);
    ++m_metrics.counters().accepted;
    if (arrival == ArrivalClass::LateWithinWindow) {
        ++m_metrics.counters().late;
    }

    Status status = ProcessReady();
    if (!status.ok()) {
        return status;
    }

    if (arrival == ArrivalClass::LateWithinWindow) {
        if (const auto window = m_scheduler.ReopenFor(eventTime)) {
            ++m_metrics.counters().windows_reopened;
            status = Solve(*window);
            if (!status.ok()) {
                return status;
            }
        }
    }

    return CommitClosedWindows(m_buffer.watermark());
}

Status MasterPipeline::ProcessReady() {
    for (SyncedTracklet& item : m_buffer.PopReady()) {
        AddToStore(std::move(item));
    }
    m_metrics.SetWatermark(m_buffer.watermark());

    for (const Window& window : m_scheduler.Advance(m_buffer.watermark())) {
        const Status status = Solve(window);
        if (!status.ok()) {
            return status;
        }
    }

    // Committing is deliberately left to the caller: a message classified as
    // `LateWithinWindow` must get a chance to reopen its window before that
    // window is frozen.
    return Status::Ok();
}

Status MasterPipeline::CommitClosedWindows(Timestamp watermark) {
    const std::vector<Window> closed = m_scheduler.AdvanceCommit(watermark);
    if (closed.empty() || !m_ids) {
        return Status::Ok();
    }

    std::set<GlobalId> ids;
    for (const Window& window : closed) {
        m_scheduler.MarkCommitted(window.index);
        ++m_metrics.counters().windows_committed;

        // Re-emit every identity touched by the window so that its final
        // update reaches the sinks at the moment the window freezes.
        for (const Tracklet& tracklet : WindowTracklets(window)) {
            const GlobalId id = m_ids->IdFor(tracklet.id);
            if (id != kInvalidGlobalId) {
                ids.insert(id);
            }
        }
    }

    // `GlobalIdManager` freezes groups that ended more than `commit_lag`
    // before `watermark`, which matches the window commit horizon.
    Status status = m_ids->Commit(watermark);
    if (!status.ok()) {
        return status;
    }

    status = EmitNewMerges();
    if (!status.ok()) {
        return status;
    }

    return EmitUpdates(std::vector<GlobalId>(ids.begin(), ids.end()), watermark, false);
}

Status MasterPipeline::Solve(const Window& window) {
    const Timestamp started = m_clock.steady ? m_clock.steady() : SteadyNowMs();

    std::vector<GlobalId> touched;
    Status status = SolveWindow(window, touched);
    if (!status.ok()) {
        return status;
    }

    const Timestamp finished = m_clock.steady ? m_clock.steady() : SteadyNowMs();
    m_metrics.RecordSolveTime(finished - started);
    ++m_metrics.counters().windows_solved;

    status = EmitNewMerges();
    if (!status.ok()) {
        return status;
    }

    status = EmitUpdates(touched, window.end, false);
    if (!status.ok()) {
        return status;
    }

    PruneRetention(m_buffer.watermark());
    return Status::Ok();
}

Status MasterPipeline::SolveWindow(const Window& window, std::vector<GlobalId>& touched) {
    touched.clear();
    const std::vector<Tracklet> windowTracklets = WindowTracklets(window);
    if (windowTracklets.empty()) {
        return Status::Ok();
    }

    for (const Tracklet& tracklet : windowTracklets) {
        if (!HasUsableEmbedding(tracklet, m_config.reid.embedding_dim)) {
            continue;
        }
        const Status added = m_gallery->Add(tracklet);
        if (!added.ok()) {
            m_logger->warn("gallery add failed for {}: {}", tracklet.id, added.ToString());
        }
    }

    const std::vector<flow::TransitionEdge> transitions = BuildTransitions(windowTracklets);
    flow::FlowAssociator associator(MakeFlowOptions(m_config));
    auto result = associator.Associate(windowTracklets, transitions);
    if (!result) {
        return result.status();
    }

    for (const Tracklet& tracklet : windowTracklets) {
        auto added = m_ids->Add(tracklet.id, tracklet.end);
        if (!added) {
            return added.status();
        }
    }

    for (const flow::FlowPath& path : result.value().paths) {
        for (std::size_t i = 1; i < path.tracklets.size(); ++i) {
            const Status linked = m_ids->Link(path.tracklets[i - 1], path.tracklets[i], window.end, "window");
            if (!linked.ok()) {
                return linked;
            }
        }
    }

    std::set<GlobalId> ids;
    for (const Tracklet& tracklet : windowTracklets) {
        const GlobalId id = m_ids->IdFor(tracklet.id);
        if (id != kInvalidGlobalId) {
            ids.insert(id);
        }
    }
    touched.assign(ids.begin(), ids.end());
    return Status::Ok();
}

std::vector<Tracklet> MasterPipeline::WindowTracklets(const Window& window) const {
    std::vector<Tracklet> out;
    const Timestamp from = window.begin - Timestamp(m_config.window_overlap_ms);

    for (const auto& [id, tracklet] : m_store) {
        (void)id;
        if (tracklet.end < from) {
            continue;
        }
        if (tracklet.begin > window.end) {
            continue;
        }
        out.push_back(tracklet);
    }

    SortTracklets(out);
    return out;
}

std::vector<flow::TransitionEdge> MasterPipeline::BuildTransitions(
    const std::vector<Tracklet>& tracklets) const {
    std::vector<flow::TransitionEdge> transitions;
    if (!m_gallery) {
        return transitions;
    }

    const int dim = m_config.reid.embedding_dim;
    std::unordered_set<TrackletId> ids;
    for (const Tracklet& tracklet : tracklets) {
        if (HasUsableEmbedding(tracklet, dim)) {
            ids.insert(tracklet.id);
        }
    }
    if (ids.empty()) {
        return transitions;
    }

    st::PairGenerationOptions options;
    options.top_k = static_cast<std::size_t>(std::max(1, m_config.reid.top_k));
    options.min_similarity = m_config.reid.min_similarity;
    options.oversample = 4;
    options.exclude_same_camera = m_config.reid.exclude_same_camera;
    options.max_total_cost = 1.0;
    options.weights = m_config.st.weights;
    options.window = m_linkModel ? m_linkModel->Fallback()
                                 : st::TransitionWindow{m_config.st.min_transition_ms,
                                                        m_config.st.max_transition_ms};
    options.gate_mode = m_config.st.gate.mode;
    options.max_speed_mps = m_config.st.gate.max_speed_mps;
    options.max_distance_m = m_config.st.gate.max_distance_m;
    options.allow_missing_world = true;

    st::PairCandidateGenerator generator(*m_gallery, m_linkModel.get(), options);
    std::map<std::pair<TrackletId, TrackletId>, double> best;

    for (const Tracklet& tracklet : tracklets) {
        if (!HasUsableEmbedding(tracklet, dim)) {
            continue;
        }
        auto candidates = generator.Generate(tracklet);
        if (!candidates) {
            m_logger->warn("pair generation failed for {}: {}", tracklet.id, candidates.status().ToString());
            continue;
        }
        for (const st::PairCandidate& candidate : candidates.value()) {
            if (ids.count(candidate.query_id) == 0 || ids.count(candidate.candidate_id) == 0) {
                continue;
            }
            const auto key = std::make_pair(candidate.query_id, candidate.candidate_id);
            const double cost = static_cast<double>(candidate.costs.total);
            const auto it = best.find(key);
            if (it == best.end() || cost < it->second) {
                best[key] = cost;
            }
        }
    }

    transitions.reserve(best.size());
    for (const auto& [key, cost] : best) {
        transitions.push_back(flow::TransitionEdge{key.first, key.second, cost});
    }
    return transitions;
}

void MasterPipeline::AddToStore(SyncedTracklet synced) {
    const TrackletId id = synced.tracklet.id;
    const RawTiming timing{synced.tracklet.camera, synced.raw_begin, synced.raw_end, synced.correction,
                           synced.received_at};

    auto it = m_store.find(id);
    if (it == m_store.end()) {
        m_store.emplace(id, std::move(synced.tracklet));
    } else if (synced.tracklet.end >= it->second.end) {
        it->second = std::move(synced.tracklet);
    }
    m_rawTiming[id] = timing;
}

void MasterPipeline::PruneRetention(Timestamp watermark) {
    if (!m_buffer.has_watermark()) {
        return;
    }

    const Timestamp horizon = watermark - Timestamp(m_config.retention_ms);
    for (auto it = m_store.begin(); it != m_store.end();) {
        if (it->second.end < horizon) {
            if (m_gallery) {
                (void)m_gallery->Remove(it->first);
            }
            m_rawTiming.erase(it->first);
            it = m_store.erase(it);
        } else {
            ++it;
        }
    }
}

Status MasterPipeline::EmitUpdates(const std::vector<GlobalId>& ids, Timestamp now, bool finalize) {
    for (const GlobalId id : ids) {
        if (id == kInvalidGlobalId) {
            continue;
        }
        const Status status = EmitUpdate(id, now, finalize);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status MasterPipeline::EmitUpdate(GlobalId id, Timestamp now, bool finalize) {
    std::vector<Tracklet> segments;
    for (const auto& [trackletId, tracklet] : m_store) {
        if (m_ids->IdFor(trackletId) == id) {
            segments.push_back(tracklet);
        }
    }
    if (segments.empty()) {
        return Status::Ok();
    }
    SortTracklets(segments);

    GlobalTrackUpdate update;
    update.global_id = id;
    update.segments = std::move(segments);
    update.type = DominantType(update.segments);
    update.is_final = finalize || m_ids->IsCommitted(id);
    update.updated_at = now;
    return Publish(update);
}

Status MasterPipeline::EmitNewMerges() {
    if (!m_ids) {
        return Status::Ok();
    }
    const std::vector<GlobalIdMergeEvent>& events = m_ids->MergeEvents();
    for (; m_emittedMerges < events.size(); ++m_emittedMerges) {
        const Status status = PublishMerge(events[m_emittedMerges]);
        if (!status.ok()) {
            return status;
        }
    }
    return Status::Ok();
}

Status MasterPipeline::Publish(const GlobalTrackUpdate& update) {
    ++m_metrics.counters().results_published;
    for (const std::unique_ptr<IResultSink>& sink : m_dependencies.sinks) {
        const Status status = sink->Publish(update);
        if (!status.ok()) {
            ++m_metrics.counters().sink_failures;
            m_logger->warn("result sink '{}' failed: {}", sink->Describe(), status.ToString());
        }
    }
    return Status::Ok();
}

Status MasterPipeline::PublishMerge(const GlobalIdMergeEvent& event) {
    ++m_metrics.counters().merges_published;
    for (const std::unique_ptr<IResultSink>& sink : m_dependencies.sinks) {
        const Status status = sink->PublishMerge(event);
        if (!status.ok()) {
            ++m_metrics.counters().sink_failures;
            m_logger->warn("result sink '{}' failed: {}", sink->Describe(), status.ToString());
        }
    }
    return Status::Ok();
}

Status MasterPipeline::FlushSinks() {
    for (const std::unique_ptr<IResultSink>& sink : m_dependencies.sinks) {
        const Status status = sink->Flush();
        if (!status.ok()) {
            ++m_metrics.counters().sink_failures;
            m_logger->warn("result sink '{}' flush failed: {}", sink->Describe(), status.ToString());
        }
    }
    return Status::Ok();
}

Status MasterPipeline::Flush() {
    for (SyncedTracklet& item : m_buffer.DrainAll()) {
        AddToStore(std::move(item));
    }

    const Timestamp end = m_max_event +
                          Timestamp(m_config.warmup_ms + m_config.window_ms + m_config.commit_lag_ms + 1);
    for (const Window& window : m_scheduler.Advance(end)) {
        const Status status = Solve(window);
        if (!status.ok()) {
            return status;
        }
    }

    Status status = CommitClosedWindows(end);
    if (!status.ok()) {
        return status;
    }

    if (m_ids) {
        const Status committed = m_ids->Commit(end);
        if (!committed.ok()) {
            return committed;
        }
    }

    status = EmitNewMerges();
    if (!status.ok()) {
        return status;
    }

    std::set<GlobalId> ids;
    for (const auto& [trackletId, tracklet] : m_store) {
        (void)tracklet;
        const GlobalId id = m_ids ? m_ids->IdFor(trackletId) : kInvalidGlobalId;
        if (id != kInvalidGlobalId) {
            ids.insert(id);
        }
    }
    status = EmitUpdates(std::vector<GlobalId>(ids.begin(), ids.end()), m_max_event, true);
    if (!status.ok()) {
        return status;
    }

    status = FlushSinks();
    if (!status.ok()) {
        return status;
    }

    UpdateCameraStats();
    m_metrics.SetQueue(0, m_store.size(), m_gallery ? m_gallery->Size() : 0);
    return Status::Ok();
}

PipelineStats MasterPipeline::Stats() {
    UpdateCameraStats();
    m_metrics.SetWatermark(m_buffer.watermark());
    m_metrics.SetQueue(m_buffer.size(), m_store.size(), m_gallery ? m_gallery->Size() : 0);
    return m_metrics.Snapshot();
}

std::optional<RawTiming> MasterPipeline::RawTimingFor(TrackletId id) const {
    const auto it = m_rawTiming.find(id);
    if (it == m_rawTiming.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::pair<TrackletId, RawTiming>> MasterPipeline::RawTimings() const {
    std::vector<std::pair<TrackletId, RawTiming>> out(m_rawTiming.begin(), m_rawTiming.end());
    std::sort(out.begin(), out.end(),
              [](const std::pair<TrackletId, RawTiming>& lhs, const std::pair<TrackletId, RawTiming>& rhs) {
                  return lhs.first < rhs.first;
              });
    return out;
}

std::vector<std::pair<TrackletId, GlobalId>> MasterPipeline::Assignments() const {
    if (!m_ids) {
        return {};
    }
    return m_ids->Assignments();
}

void MasterPipeline::UpdateCameraStats() {
    std::vector<CameraClockStats> cameras;
    cameras.reserve(m_clockSync.size());
    for (const auto& [camera, estimator] : m_clockSync) {
        const ClockSyncStats stats = estimator.Stats();
        CameraClockStats entry;
        entry.camera = camera;
        entry.offset_ms = stats.offset_ms;
        entry.drift_ppm = stats.drift_ppm;
        entry.residual_ms = stats.residual_ms;
        entry.samples = stats.samples;
        entry.synced = stats.synced;
        cameras.push_back(std::move(entry));
    }
    std::sort(cameras.begin(), cameras.end(),
              [](const CameraClockStats& lhs, const CameraClockStats& rhs) { return lhs.camera < rhs.camera; });
    m_metrics.SetCameras(std::move(cameras));
}

} // namespace tmcmt::pipeline
