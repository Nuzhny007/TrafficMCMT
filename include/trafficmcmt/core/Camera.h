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

    [[nodiscard]] bool IsValid() const noexcept {
        if (camera_id.empty()) {
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
        // Degenerate homography: last element must not be zero.
        return homography[8] != 0.0;
    }
};

} // namespace tmcmt
