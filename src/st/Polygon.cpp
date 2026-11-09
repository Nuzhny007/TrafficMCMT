#include "trafficmcmt/st/Polygon.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tmcmt::st {
namespace {

constexpr double kEpsilon = 1e-9;

bool OnSegment(const EnuPoint& a, const EnuPoint& b, double east, double north) noexcept {
    const double cross = (east - a.east) * (b.north - a.north) -
                         (north - a.north) * (b.east - a.east);
    if (std::abs(cross) > kEpsilon) {
        return false;
    }
    const double minEast = std::min(a.east, b.east) - kEpsilon;
    const double maxEast = std::max(a.east, b.east) + kEpsilon;
    const double minNorth = std::min(a.north, b.north) - kEpsilon;
    const double maxNorth = std::max(a.north, b.north) + kEpsilon;
    return east >= minEast && east <= maxEast && north >= minNorth && north <= maxNorth;
}

} // namespace

bool Polygon::Contains(double east, double north) const noexcept {
    if (!IsValid() || !std::isfinite(east) || !std::isfinite(north)) {
        return false;
    }

    const std::size_t count = points.size();
    bool inside = false;
    for (std::size_t i = 0, previous = count - 1; i < count; previous = i++) {
        const EnuPoint& a = points[previous];
        const EnuPoint& b = points[i];
        if (!a.IsFinite() || !b.IsFinite()) {
            return false;
        }
        if (OnSegment(a, b, east, north)) {
            return true;
        }
        const bool crosses = (a.north > north) != (b.north > north);
        if (!crosses) {
            continue;
        }
        const double intersectionEast =
            a.east + (north - a.north) * (b.east - a.east) / (b.north - a.north);
        if (east < intersectionEast) {
            inside = !inside;
        }
    }
    return inside;
}

bool Polygon::Contains(const EnuPoint& point) const noexcept {
    return Contains(point.east, point.north);
}

} // namespace tmcmt::st
