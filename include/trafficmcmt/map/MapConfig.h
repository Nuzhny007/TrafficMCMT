#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

struct MapCameraConfig {
    CameraId id;
    int camera_id = 0;
    std::array<int, 4> color{148, 163, 184, 55};
    /// Zone polygon in `[lon, lat]` order. Empty means "no zone".
    std::vector<GeoPoint> polygon;
    /// Source `geo_bind.ini` used to derive `polygon` at load time.
    std::string geo_bind;
};

struct MapFeedConfig {
    bool has_origin = false;
    double origin_latitude = 0.0;
    double origin_longitude = 0.0;

    int tick_hz = 10;
    std::size_t trail_points = 40;
    double trail_min_distance_m = 0.3;
    double velocity_window_s = 1.0;
    double velocity_max_mps = 70.0;
    double track_ttl_s = 3.0;
    /// 0 disables periodic snapshots (snapshots are still sent on connect).
    double snapshot_interval_s = 0.0;
    double heartbeat_s = 5.0;

    std::vector<MapCameraConfig> cameras;

    [[nodiscard]] const MapCameraConfig* FindCamera(const CameraId& id) const noexcept;
    /// Numeric camera id, or -1 when the camera is not configured.
    [[nodiscard]] int CameraNumber(const CameraId& id) const noexcept;

    [[nodiscard]] GeoPoint Origin() const noexcept {
        return GeoPoint{origin_latitude, origin_longitude};
    }
};

/// Loads `configs/map_feed.yaml`.
[[nodiscard]] Result<MapFeedConfig> LoadMapFeedConfig(const std::string& path);

/// Parses the `[points]` section of a Multitarget-tracker `geo_bind.ini` and
/// returns the convex hull of the control points as a `[lon, lat]` polygon.
/// Implemented without OpenCV so the map library stays lightweight.
[[nodiscard]] Result<std::vector<GeoPoint>> LoadGeoBindingPolygon(const std::string& path);

struct MapOriginResolution {
    GeoPoint origin{};
    /// True when the origin was read from the tracklet stream.
    bool from_stream = false;
};

/// Resolves the shared ENU origin for a batch of tracklets.
///
/// * every tracklet that carries an origin must agree with the others;
/// * a configured origin must agree with the stream;
/// * at least one of the two sources must be present.
[[nodiscard]] Result<MapOriginResolution> ResolveMapOrigin(const std::vector<Tracklet>& tracklets,
                                                           const MapFeedConfig& config);

} // namespace tmcmt::map
