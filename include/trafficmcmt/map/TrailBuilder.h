#pragma once

#include <cstddef>
#include <vector>

#include "trafficmcmt/core/Geo.h"

namespace tmcmt::map {

/// Bounded ENU trail with distance-based decimation.
class TrailBuilder {
public:
    TrailBuilder(std::size_t max_points, double min_distance_m);

    void Add(const EnuPoint& point);

    [[nodiscard]] const std::vector<EnuPoint>& Points() const noexcept {
        return m_points;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return m_points.empty();
    }

    void Clear();

private:
    std::size_t m_maxPoints = 40;
    double m_minDistanceSq = 0.0;
    std::vector<EnuPoint> m_points;
};

} // namespace tmcmt::map
