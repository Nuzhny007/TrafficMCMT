#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt {

struct EdgeExportConfig {
    CameraId camera_id = "cam-01";
    /// UUID of the Edge run. Must be stable for the lifetime of the process.
    std::string session_id = "session-01";
    /// Emit a periodic chunk every `chunk_interval_ms` of track time.
    std::int64_t chunk_interval_ms = 1000;
    /// Do not emit a chunk with fewer observations (except on Flush).
    std::size_t min_observations = 2;
    std::uint32_t schema_version = 1;
    /// Used when the extractor does not set `Embedding::model_id`.
    std::uint32_t model_id = 0;
    /// Shared ENU origin written into every chunk. Zero means "not provided".
    double origin_latitude = 0.0;
    double origin_longitude = 0.0;
};

/// Aggregates per-track observations into `Tracklet` chunks.
///
/// The exporter is transport-agnostic and MTT-agnostic: `MttTrackAdapter`
/// feeds it observations, `apps/mtt_export` writes the result to a file, and
/// the ZeroMQ publisher (stage 6) will send the same chunks over the network.
class EdgeTrackExporter {
public:
    explicit EdgeTrackExporter(EdgeExportConfig config);

    /// Adds one observation and returns zero or more ready chunks.
    [[nodiscard]] std::vector<Tracklet> AddObservation(std::uint64_t local_track_id, const Observation& observation,
                                                       const Embedding& embedding, ObjectType type,
                                                       float confidence, Timestamp now);

    /// Emits periodic chunks for tracks whose interval has elapsed.
    [[nodiscard]] std::vector<Tracklet> Tick(Timestamp now);

    /// Emits the final chunk for every open track and clears the state.
    [[nodiscard]] std::vector<Tracklet> Flush();

    void Reset();

    [[nodiscard]] std::size_t OpenTracks() const noexcept {
        return m_tracks.size();
    }

    [[nodiscard]] const EdgeExportConfig& Config() const noexcept {
        return m_config;
    }

private:
    struct TrackState {
        std::uint64_t local_track_id = 0;
        ObjectType type = ObjectType::Unknown;
        Timestamp begin{};
        Timestamp end{};
        Timestamp last_emit{};
        std::uint64_t next_sequence = 0;
        std::vector<Observation> pending;
        std::vector<float> embedding_sum;
        std::uint32_t model_id = 0;
        std::size_t embedding_count = 0;
        std::size_t total_observations = 0;
        float max_confidence = 0.f;
    };

    [[nodiscard]] std::vector<Tracklet> Emit(TrackState& state, Timestamp now, bool final);

    EdgeExportConfig m_config;
    std::unordered_map<std::uint64_t, TrackState> m_tracks;
};

} // namespace tmcmt
