#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// Polygon in image coordinates stored as a flat [x0, y0, x1, y1, ...] list.
struct Polygon {
    std::vector<float> xy;

    [[nodiscard]] bool IsValid() const noexcept {
        return xy.size() >= 6 && xy.size() % 2 == 0;
    }

    [[nodiscard]] std::size_t PointCount() const noexcept {
        return xy.size() / 2;
    }
};

/// Row-major 3x3 homography: image plane -> ground plane.
using Homography = std::array<double, 9>;

struct CameraCalibration {
    CameraId camera_id;
    bool has_homography = false;
    Homography homography{};
    Polygon entry_zone;
    Polygon exit_zone;
    float meters_per_pixel_at_ground = 0.f;
    std::string frame_id = "map";
    std::int64_t updated_at_ms = 0;

    /// WGS-84 origin of the metric frame produced by `homography`.
    /// Zero means "not provided"; the consumer then falls back to the
    /// tracklet origin or the shared configuration origin.
    double origin_latitude = 0.0;
    double origin_longitude = 0.0;

    /// Frame size the calibration was made for. Required for pixel-space
    /// homographies, ignored for normalised ones.
    int image_width = 0;
    int image_height = 0;

    /// true: homography maps normalised pixels [0, 1] -> metres.
    /// false: homography maps raw pixels -> metres.
    bool homography_normalized = true;

    /// RMS reprojection error of the control points, metres.
    float reprojection_error_m = 0.f;

    /// "manual" or "estimated".
    std::string source = "manual";

    /// Monotonic per-camera version used by the hot-reload store.
    std::uint64_t version = 0;

    [[nodiscard]] bool HasOrigin() const noexcept {
        return std::isfinite(origin_latitude) && std::isfinite(origin_longitude) &&
               (origin_latitude != 0.0 || origin_longitude != 0.0);
    }

    [[nodiscard]] bool IsValid() const noexcept {
        if (camera_id.empty()) {
            return false;
        }
        if (!std::isfinite(origin_latitude) || !std::isfinite(origin_longitude)) {
            return false;
        }
        if (!has_homography) {
            return true;
        }
        for (const double value : homography) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        if (!homography_normalized && (image_width <= 0 || image_height <= 0)) {
            return false;
        }
        // Degenerate homography: last element must not be zero.
        return homography[8] != 0.0;
    }
};

} // namespace tmcmt
