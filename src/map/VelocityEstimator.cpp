#include "trafficmcmt/map/VelocityEstimator.h"

#include <algorithm>
#include <cmath>

namespace tmcmt::map {
namespace {

constexpr std::int64_t kStaleMs = 1000;
constexpr double kMinFiniteDifferenceSpanS = 0.15;

} // namespace

VelocityEstimator::VelocityEstimator(double window_s, double max_mps)
    : m_window_s(std::max(0.1, window_s)), m_max_mps(std::max(0.1, max_mps)) {}

void VelocityEstimator::Add(Timestamp ts, const EnuPoint& point) {
    if (!point.IsFinite()) {
        return;
    }
    if (!m_samples.empty() && ts < m_samples.back().ts) {
        return;
    }
    if (!m_samples.empty() && ts == m_samples.back().ts) {
        m_samples.back().point = point;
        return;
    }

    m_samples.push_back(Sample{ts, point});

    const Timestamp cutoff = ts - Timestamp(static_cast<std::int64_t>(m_window_s * 1000.0));
    while (m_samples.size() > 2 && m_samples.front().ts < cutoff) {
        m_samples.pop_front();
    }
}

std::optional<EnuPoint> VelocityEstimator::Estimate(Timestamp now) const {
    if (m_samples.size() < 2) {
        return std::nullopt;
    }
    if (now - m_samples.back().ts > Timestamp(kStaleMs)) {
        return std::nullopt;
    }

    const double t0 = static_cast<double>(m_samples.front().ts.count()) / 1000.0;
    const double tLast = static_cast<double>(m_samples.back().ts.count()) / 1000.0;
    const double span = tLast - t0;

    EnuPoint velocity;
    if (span < kMinFiniteDifferenceSpanS) {
        const Sample& first = m_samples[m_samples.size() - 2];
        const Sample& last = m_samples.back();
        const double dt = static_cast<double>((last.ts - first.ts).count()) / 1000.0;
        if (!(dt > 0.0)) {
            return std::nullopt;
        }
        velocity.east = (last.point.east - first.point.east) / dt;
        velocity.north = (last.point.north - first.point.north) / dt;
    } else {
        const double n = static_cast<double>(m_samples.size());
        double sumT = 0.0;
        double sumE = 0.0;
        double sumN = 0.0;
        double sumTT = 0.0;
        double sumTE = 0.0;
        double sumTN = 0.0;
        for (const Sample& sample : m_samples) {
            const double t = static_cast<double>(sample.ts.count()) / 1000.0 - t0;
            sumT += t;
            sumE += sample.point.east;
            sumN += sample.point.north;
            sumTT += t * t;
            sumTE += t * sample.point.east;
            sumTN += t * sample.point.north;
        }
        const double denominator = n * sumTT - sumT * sumT;
        if (std::fabs(denominator) < 1e-9) {
            return std::nullopt;
        }
        velocity.east = (n * sumTE - sumT * sumE) / denominator;
        velocity.north = (n * sumTN - sumT * sumN) / denominator;
    }

    if (!velocity.IsFinite()) {
        return std::nullopt;
    }

    const double magnitude = std::hypot(velocity.east, velocity.north);
    if (magnitude > m_max_mps) {
        const double scale = m_max_mps / magnitude;
        velocity.east *= scale;
        velocity.north *= scale;
    }
    return velocity;
}

void VelocityEstimator::Clear() {
    m_samples.clear();
}

} // namespace tmcmt::map
