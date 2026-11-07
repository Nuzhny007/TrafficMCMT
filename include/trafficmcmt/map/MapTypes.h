#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

/// Largest integer that survives a round-trip through a JavaScript `number`.
inline constexpr std::uint64_t kMaxJsSafeId = (1ULL << 53) - 1;

enum class MapFrameType : std::uint8_t {
    TracksUpdate = 0,
    CameraZones = 1,
    Heartbeat = 2,
    IdMerge = 3,
    TracksRemove = 4,
    EmissionGrid = 5,
};

/// One track inside `tracks_update`.
struct MapTrackUpdate {
    std::uint64_t global_id = 0;
    int camera_id = 0;
    std::string class_name = "unknown";
    GeoPoint position{};
    EnuPoint velocity{};
    bool has_velocity = false;
    double speed_kmh = 0.0;
    /// Empty means "omit": the viewer accumulates the trail itself.
    std::vector<GeoPoint> trail;
};

struct MapCameraZone {
    int camera_id = 0;
    std::vector<GeoPoint> polygon;
    std::array<int, 4> color{148, 163, 184, 55};
};

/// One pollutant raster inside `emission_grid`.
struct MapEmissionField {
    /// Row-major, south -> north, in ug/m3.
    std::vector<float> values;
    float max = 0.f;
    float mean = 0.f;
    float p95 = 0.f;
};

/// Additive protocol-v1 frame carrying the concentration map.
struct MapEmissionGrid {
    GeoPoint south_west{};
    GeoPoint north_east{};
    int cols = 0;
    int rows = 0;
    /// Indexed by `emissions::Pollutant`: NOx, CO, PM2.5, PM10, CO2.
    std::array<MapEmissionField, 5> fields;
};

/// Protocol v1 frame. The JSON writer lives in `MapProtocol.cpp`.
struct MapFrame {
    MapFrameType type = MapFrameType::TracksUpdate;
    Timestamp timestamp{};
    bool snapshot = false;
    std::vector<MapTrackUpdate> tracks;
    std::vector<MapCameraZone> zones;
    std::vector<std::uint64_t> removed_ids;
    std::uint64_t old_id = 0;
    std::uint64_t new_id = 0;
    std::optional<MapEmissionGrid> emission;
};

/// Viewer class name for a protocol object type (`person`, not `pedestrian`).
[[nodiscard]] const char* MapClassName(ObjectType type) noexcept;

/// Serialises a frame as one compact JSON object (protocol v1).
[[nodiscard]] std::string ToJson(const MapFrame& frame);

} // namespace tmcmt::map
