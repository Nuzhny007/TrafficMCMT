#pragma once

#include <vector>

#include "trafficmcmt/core/Geo.h"

namespace tmcmt::st {

/// Simple polygon in the shared ENU plane (metres).
///
/// Zones are defined in metres rather than image coordinates because the
/// tracklet protocol does not carry the frame size; see docs/architecture.md
/// §8.5. Points on an edge count as inside.
struct Polygon {
    std::vector<EnuPoint> points;

    [[nodiscard]] bool IsValid() const noexcept {
        return points.size() >= 3;
    }

    [[nodiscard]] bool Contains(const EnuPoint& point) const noexcept;
    [[nodiscard]] bool Contains(double east, double north) const noexcept;
};

} // namespace tmcmt::st
