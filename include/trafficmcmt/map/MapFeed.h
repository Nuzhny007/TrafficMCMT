#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/map/MapConfig.h"
#include "trafficmcmt/map/MapIdRegistry.h"
#include "trafficmcmt/map/MapTypes.h"
#include "trafficmcmt/map/TrailBuilder.h"
#include "trafficmcmt/map/VelocityEstimator.h"

namespace tmcmt::map {

/// Converts a stream of Edge observations into protocol-v1 map frames.
///
/// The feed is transport-agnostic: `apps/map_export` writes frames to a file,
/// `apps/map_gateway` broadcasts them over WebSocket, and unit tests inspect
/// the typed `MapFrame` values directly.
class MapFeed {
public:
    explicit MapFeed(MapFeedConfig config);

    /// Consumes one observation. Invalid world points and unknown cameras are
    /// skipped; the latter are counted in `UnknownCameraObservations()`.
    void Observe(const Tracklet& tracklet, const Observation& observation);

    /// Emits a delta frame for updated tracks and a `tracks_remove` frame for
    /// tracks whose last observation is older than `track_ttl_s`.
    [[nodiscard]] std::vector<MapFrame> Tick(Timestamp now);

    /// Full state for a newly connected client (mode = snapshot).
    [[nodiscard]] MapFrame Snapshot(Timestamp now) const;

    /// Static camera zones; sent once per connection.
    [[nodiscard]] MapFrame CameraZones(Timestamp now) const;

    [[nodiscard]] MapFrame Heartbeat(Timestamp now) const;

    /// Re-keys a live track after a global identity merge.
    [[nodiscard]] std::optional<MapFrame> Rename(std::uint64_t old_id, std::uint64_t new_id);

    [[nodiscard]] std::size_t ActiveTracks() const noexcept {
        return m_tracks.size();
    }

    [[nodiscard]] std::size_t UnknownCameraObservations() const noexcept {
        return m_unknownCameraObservations;
    }

    [[nodiscard]] const MapFeedConfig& Config() const noexcept {
        return m_config;
    }

    [[nodiscard]] const MapIdRegistry& Ids() const noexcept {
        return m_ids;
    }

private:
    struct TrackState {
        TrackState(std::uint64_t id, int camera, std::string className, const MapFeedConfig& config);

        std::uint64_t global_id = 0;
        int camera_id = 0;
        std::string class_name = "unknown";
        Timestamp last_seen{};
        TrailBuilder trail;
        VelocityEstimator velocity;
        bool dirty = false;
        bool announced = false;
    };

    [[nodiscard]] MapTrackUpdate BuildUpdate(const TrackState& state, Timestamp now) const;

    MapFeedConfig m_config;
    GeoPoint m_origin{};
    MapIdRegistry m_ids;
    std::unordered_map<std::uint64_t, TrackState> m_tracks;
    std::size_t m_unknownCameraObservations = 0;
};

} // namespace tmcmt::map
