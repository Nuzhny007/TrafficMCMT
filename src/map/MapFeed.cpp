#include "trafficmcmt/map/MapFeed.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tmcmt::map {
namespace {

MapFrame MakeEmissionFrame(const emissions::EmissionGridSnapshot& snapshot) {
    MapFrame frame;
    frame.type = MapFrameType::EmissionGrid;
    frame.timestamp = snapshot.timestamp;

    MapEmissionGrid grid;
    grid.south_west = snapshot.south_west;
    grid.north_east = snapshot.north_east;
    grid.cols = snapshot.cols;
    grid.rows = snapshot.rows;
    for (std::size_t i = 0; i < grid.fields.size(); ++i) {
        grid.fields[i].values = snapshot.fields[i];
        grid.fields[i].max = snapshot.stats[i].max;
        grid.fields[i].mean = snapshot.stats[i].mean;
        grid.fields[i].p95 = snapshot.stats[i].p95;
    }

    frame.emission = std::move(grid);
    return frame;
}

} // namespace

MapFeed::TrackState::TrackState(std::uint64_t id, int camera, std::string className, const MapFeedConfig& config)
    : global_id(id),
      camera_id(camera),
      class_name(std::move(className)),
      trail(config.trail_points, config.trail_min_distance_m),
      velocity(config.velocity_window_s, config.velocity_max_mps),
      acceleration(config.acceleration_window_s, config.acceleration_max_mps2) {}

MapFeed::MapFeed(MapFeedConfig config) : m_config(std::move(config)), m_origin(m_config.Origin()) {
    if (m_config.emissions.enabled) {
        m_emissions.emplace(m_config.emissions, m_origin);
    }
}

void MapFeed::Observe(const Tracklet& tracklet, const Observation& observation) {
    if (!observation.world.valid || !observation.world.IsFinite()) {
        return;
    }

    const int camera = m_config.CameraNumber(tracklet.camera);
    if (camera < 0) {
        ++m_unknownCameraObservations;
        return;
    }

    const TrackKey key{tracklet.session_id, tracklet.camera, tracklet.local_track_id};
    const std::uint64_t id = m_ids.Assign(key);

    auto [it, inserted] =
        m_tracks.try_emplace(id, id, camera, MapClassName(tracklet.type), m_config);
    TrackState& state = it->second;
    if (!inserted && tracklet.type != ObjectType::Unknown) {
        state.class_name = MapClassName(tracklet.type);
    }

    const EnuPoint point{observation.world.x, observation.world.y};
    state.trail.Add(point);
    state.velocity.Add(observation.ts, point);
    if (const std::optional<EnuPoint> velocity = state.velocity.Estimate(observation.ts)) {
        state.acceleration.Add(observation.ts, std::hypot(velocity->east, velocity->north));
    }
    state.last_seen = std::max(state.last_seen, observation.ts);
    state.dirty = true;
}

MapTrackUpdate MapFeed::BuildUpdate(const TrackState& state, Timestamp now) const {
    MapTrackUpdate update;
    update.global_id = state.global_id;
    update.camera_id = state.camera_id;
    update.class_name = state.class_name;

    const std::vector<EnuPoint>& points = state.trail.Points();
    if (!points.empty()) {
        update.position = EnuToGeographic(points.back(), m_origin);
    }

    const std::optional<EnuPoint> velocity = state.velocity.Estimate(now);
    if (velocity) {
        update.has_velocity = true;
        update.velocity = *velocity;
        update.speed_kmh = std::hypot(velocity->east, velocity->north) * 3.6;
    }

    if (!state.announced) {
        update.trail.reserve(points.size());
        for (const EnuPoint& point : points) {
            update.trail.push_back(EnuToGeographic(point, m_origin));
        }
    }
    return update;
}

std::vector<MapFrame> MapFeed::Tick(Timestamp now) {
    std::vector<MapFrame> frames;

    // Expire first so a track that is about to disappear never produces a
    // final `tracks_update` in the same tick.
    std::vector<std::uint64_t> removed;
    const Timestamp ttl(static_cast<std::int64_t>(m_config.track_ttl_s * 1000.0));
    for (auto it = m_tracks.begin(); it != m_tracks.end();) {
        if (now - it->second.last_seen > ttl) {
            removed.push_back(it->first);
            it = m_tracks.erase(it);
        } else {
            ++it;
        }
    }

    MapFrame update;
    update.type = MapFrameType::TracksUpdate;
    update.timestamp = now;
    update.snapshot = false;

    for (auto& [id, state] : m_tracks) {
        (void)id;
        if (!state.dirty) {
            continue;
        }
        update.tracks.push_back(BuildUpdate(state, now));
        state.dirty = false;
        state.announced = true;
    }
    if (!update.tracks.empty()) {
        frames.push_back(std::move(update));
    }

    if (!removed.empty()) {
        MapFrame frame;
        frame.type = MapFrameType::TracksRemove;
        frame.timestamp = now;
        frame.removed_ids = std::move(removed);
        frames.push_back(std::move(frame));
    }

    if (m_emissions) {
        std::vector<emissions::EmissionSource> sources;
        sources.reserve(m_tracks.size());
        for (const auto& [id, state] : m_tracks) {
            (void)id;
            const std::vector<EnuPoint>& points = state.trail.Points();
            if (points.empty()) {
                continue;
            }

            emissions::EmissionSource source;
            source.track_id = state.global_id;
            source.position = points.back();
            if (const std::optional<EnuPoint> velocity = state.velocity.Estimate(now)) {
                source.speed_mps = std::hypot(velocity->east, velocity->north);
            }
            source.acceleration_mps2 = state.acceleration.Estimate(now).value_or(0.0);
            source.class_name = state.class_name;
            sources.push_back(source);
        }

        m_emissions->Step(now, sources);
        if (m_emissions->GridDue(now)) {
            if (std::optional<emissions::EmissionGridSnapshot> snapshot = m_emissions->BuildSnapshot(now)) {
                frames.push_back(MakeEmissionFrame(*snapshot));
            }
        }
    }

    return frames;
}

MapFrame MapFeed::Snapshot(Timestamp now) const {
    MapFrame frame;
    frame.type = MapFrameType::TracksUpdate;
    frame.timestamp = now;
    frame.snapshot = true;

    for (const auto& [id, state] : m_tracks) {
        (void)id;
        MapTrackUpdate update = BuildUpdate(state, now);
        update.trail.clear();
        update.trail.reserve(state.trail.Points().size());
        for (const EnuPoint& point : state.trail.Points()) {
            update.trail.push_back(EnuToGeographic(point, m_origin));
        }
        frame.tracks.push_back(std::move(update));
    }
    return frame;
}

MapFrame MapFeed::CameraZones(Timestamp now) const {
    MapFrame frame;
    frame.type = MapFrameType::CameraZones;
    frame.timestamp = now;

    for (const MapCameraConfig& camera : m_config.cameras) {
        if (camera.polygon.size() < 3) {
            continue;
        }
        MapCameraZone zone;
        zone.camera_id = camera.camera_id;
        zone.polygon = camera.polygon;
        zone.color = camera.color;
        frame.zones.push_back(std::move(zone));
    }
    return frame;
}

std::optional<MapFrame> MapFeed::EmissionSnapshot(Timestamp now) {
    if (!m_emissions) {
        return std::nullopt;
    }
    std::optional<emissions::EmissionGridSnapshot> snapshot = m_emissions->BuildSnapshot(now);
    if (!snapshot) {
        return std::nullopt;
    }
    return MakeEmissionFrame(*snapshot);
}

MapFrame MapFeed::Heartbeat(Timestamp now) const {
    MapFrame frame;
    frame.type = MapFrameType::Heartbeat;
    frame.timestamp = now;
    return frame;
}

std::optional<MapFrame> MapFeed::Rename(std::uint64_t old_id, std::uint64_t new_id) {
    if (old_id == new_id || new_id == 0 || new_id > kMaxJsSafeId) {
        return std::nullopt;
    }
    const auto it = m_tracks.find(old_id);
    if (it == m_tracks.end() || m_tracks.find(new_id) != m_tracks.end()) {
        return std::nullopt;
    }
    if (!m_ids.Rename(old_id, new_id)) {
        return std::nullopt;
    }

    TrackState state = std::move(it->second);
    m_tracks.erase(it);
    state.global_id = new_id;
    state.dirty = true;
    state.announced = false;

    MapFrame frame;
    frame.type = MapFrameType::IdMerge;
    frame.timestamp = state.last_seen;
    frame.old_id = old_id;
    frame.new_id = new_id;

    m_tracks.emplace(new_id, std::move(state));
    return frame;
}

} // namespace tmcmt::map
