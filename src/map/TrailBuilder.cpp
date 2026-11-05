#include "trafficmcmt/map/TrailBuilder.h"

#include <algorithm>
#include <cmath>

namespace tmcmt::map {

TrailBuilder::TrailBuilder(std::size_t max_points, double min_distance_m)
    : m_maxPoints(std::max<std::size_t>(1, max_points)),
      m_minDistanceSq(std::max(0.0, min_distance_m) * std::max(0.0, min_distance_m)) {}

void TrailBuilder::Add(const EnuPoint& point) {
    if (!point.IsFinite()) {
        return;
    }
    if (!m_points.empty()) {
        const EnuPoint& last = m_points.back();
        const double dx = point.east - last.east;
        const double dy = point.north - last.north;
        if (dx * dx + dy * dy < m_minDistanceSq) {
            return;
        }
    }
    m_points.push_back(point);
    if (m_points.size() > m_maxPoints) {
        m_points.erase(m_points.begin(), m_points.begin() + static_cast<std::ptrdiff_t>(m_points.size() - m_maxPoints));
    }
}

void TrailBuilder::Clear() {
    m_points.clear();
}

} // namespace tmcmt::map
