#include "trafficmcmt/edge/EdgeTrackExporter.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "trafficmcmt/core/TrackletId.h"

namespace tmcmt {
namespace {

void AccumulateEmbedding(std::vector<float>& sum, std::size_t& count, std::uint32_t& model_id,
                         const Embedding& embedding) {
    if (!embedding.IsValid()) {
        return;
    }
    const float norm = embedding.Norm();
    if (!(norm > 0.f)) {
        return;
    }
    if (count > 0 && embedding.model_id != model_id) {
        // Never mix descriptors from different ReID models.
        return;
    }
    if (sum.size() != embedding.values.size()) {
        sum.assign(embedding.values.size(), 0.f);
        count = 0;
    }
    for (std::size_t i = 0; i < embedding.values.size(); ++i) {
        sum[i] += embedding.values[i] / norm;
    }
    model_id = embedding.model_id;
    ++count;
}

} // namespace

EdgeTrackExporter::EdgeTrackExporter(EdgeExportConfig config) : m_config(std::move(config)) {
    m_config.chunk_interval_ms = std::max<std::int64_t>(0, m_config.chunk_interval_ms);
    m_config.min_observations = std::max<std::size_t>(1, m_config.min_observations);
    if (m_config.schema_version == 0) {
        m_config.schema_version = 1;
    }
}

void EdgeTrackExporter::Reset() {
    m_tracks.clear();
}

std::vector<Tracklet> EdgeTrackExporter::AddObservation(std::uint64_t local_track_id,
                                                        const Observation& observation, const Embedding& embedding,
                                                        ObjectType type, float confidence, Timestamp now) {
    if (m_config.session_id.empty() || m_config.camera_id.empty()) {
        return {};
    }

    auto [it, inserted] = m_tracks.try_emplace(local_track_id);
    TrackState& state = it->second;
    if (inserted) {
        state.local_track_id = local_track_id;
        state.begin = observation.ts;
        state.end = observation.ts;
        // Anchor the first chunk on the track's own clock so that a track
        // that starts in the middle of an interval is emitted on time.
        state.last_emit = state.begin;
        state.type = type;
    } else {
        state.begin = std::min(state.begin, observation.ts);
        state.end = std::max(state.end, observation.ts);
        if (type != ObjectType::Unknown) {
            state.type = type;
        }
    }

    state.pending.push_back(observation);
    ++state.total_observations;
    state.max_confidence = std::max(state.max_confidence, std::isfinite(confidence) ? confidence : 0.f);
    AccumulateEmbedding(state.embedding_sum, state.embedding_count, state.model_id, embedding);

    if (now - state.last_emit >= Timestamp(m_config.chunk_interval_ms)) {
        return Emit(state, now, false);
    }
    return {};
}

std::vector<Tracklet> EdgeTrackExporter::Tick(Timestamp now) {
    std::vector<Tracklet> chunks;
    for (auto& [localId, state] : m_tracks) {
        (void)localId;
        if (state.pending.empty()) {
            continue;
        }
        if (now - state.last_emit >= Timestamp(m_config.chunk_interval_ms)) {
            auto emitted = Emit(state, now, false);
            chunks.insert(chunks.end(), std::make_move_iterator(emitted.begin()),
                          std::make_move_iterator(emitted.end()));
        }
    }
    return chunks;
}

std::vector<Tracklet> EdgeTrackExporter::Flush() {
    std::vector<Tracklet> chunks;
    for (auto& [localId, state] : m_tracks) {
        (void)localId;
        auto emitted = Emit(state, state.end, true);
        chunks.insert(chunks.end(), std::make_move_iterator(emitted.begin()), std::make_move_iterator(emitted.end()));
    }
    m_tracks.clear();
    return chunks;
}

std::vector<Tracklet> EdgeTrackExporter::Emit(TrackState& state, Timestamp now, bool final) {
    std::vector<Tracklet> chunks;
    if (state.pending.empty()) {
        state.last_emit = now;
        return chunks;
    }
    if (!final && state.pending.size() < m_config.min_observations) {
        return chunks;
    }
    if (state.embedding_count == 0 || state.embedding_sum.empty()) {
        // A tracklet without an appearance descriptor cannot be matched by the
        // Master, so it is dropped rather than sent with a fake embedding.
        state.pending.clear();
        state.last_emit = now;
        return chunks;
    }

    Tracklet tracklet;
    tracklet.session_id = m_config.session_id;
    tracklet.camera = m_config.camera_id;
    tracklet.local_track_id = state.local_track_id;
    tracklet.sequence = state.next_sequence++;
    tracklet.begin = state.begin;
    tracklet.end = state.end;
    tracklet.observations = std::move(state.pending);
    state.pending.clear();
    tracklet.type = state.type;
    tracklet.schema_version = m_config.schema_version;
    tracklet.is_end = final;
    tracklet.origin_latitude = m_config.origin_latitude;
    tracklet.origin_longitude = m_config.origin_longitude;
    const std::size_t chunkSize = tracklet.observations.size();

    tracklet.embedding.values = state.embedding_sum;
    const float inv = 1.f / static_cast<float>(state.embedding_count);
    float norm = 0.f;
    for (float& value : tracklet.embedding.values) {
        value *= inv;
        norm += value * value;
    }
    norm = std::sqrt(norm);
    if (norm > 0.f) {
        for (float& value : tracklet.embedding.values) {
            value /= norm;
        }
    }
    tracklet.embedding.dim = static_cast<std::uint16_t>(tracklet.embedding.values.size());
    tracklet.embedding.model_id = state.model_id != 0 ? state.model_id : m_config.model_id;

    const float density = std::min(1.f, static_cast<float>(chunkSize) / 20.f);
    const float confidence = std::clamp(state.max_confidence, 0.f, 1.f);
    tracklet.quality = std::clamp(0.5f * density + 0.5f * confidence, 0.f, 1.f);

    AssignTrackletId(tracklet);
    chunks.push_back(std::move(tracklet));

    state.embedding_sum.clear();
    state.embedding_count = 0;
    state.begin = state.end;
    state.last_emit = now;
    return chunks;
}

} // namespace tmcmt
