#include "trafficmcmt/map/AccelerationEstimator.h"

#include <algorithm>
#include <cmath>

namespace tmcmt::map {
namespace {

constexpr std::int64_t kStaleMs = 1000;
constexpr double kMillisecondsPerSecond = 1000.0;

} // namespace

AccelerationEstimator::AccelerationEstimator(double window_s, double max_mps2, double smoothing)
    : m_window_s(std::max(0.2, window_s)),
      m_max_mps2(std::max(0.1, max_mps2)),
      m_smoothing(std::clamp(smoothing, 0.01, 1.0)) {}

void AccelerationEstimator::Add(Timestamp ts, double speed_mps) {
    if (!std::isfinite(speed_mps)) {
        return;
    }
    if (!m_samples.empty() && ts < m_samples.back().ts) {
        return;
    }
    if (!m_samples.empty() && ts == m_samples.back().ts) {
        m_samples.back().speed_mps = speed_mps;
    } else {
        m_samples.push_back(Sample{ts, speed_mps});
    }

    const Timestamp cutoff = ts - Timestamp(static_cast<std::int64_t>(m_window_s * kMillisecondsPerSecond));
    while (m_samples.size() > 2 && m_samples.front().ts < cutoff) {
        m_samples.pop_front();
    }

    if (m_samples.size() < 2) {
        return;
    }

    const double slope = std::clamp(FitSlope(), -m_max_mps2, m_max_mps2);
    m_smoothed = m_smoothed ? (1.0 - m_smoothing) * *m_smoothed + m_smoothing * slope : slope;
    m_last_ts = ts;
}

std::optional<double> AccelerationEstimator::Estimate(Timestamp now) const {
    if (!m_smoothed || m_samples.empty() || now - m_last_ts > Timestamp(kStaleMs)) {
        return std::nullopt;
    }
    return m_smoothed;
}

double AccelerationEstimator::FitSlope() const noexcept {
    const double count = static_cast<double>(m_samples.size());
    double sum_t = 0.0;
    double sum_v = 0.0;
    double sum_tt = 0.0;
    double sum_tv = 0.0;

    const double origin = static_cast<double>(m_samples.front().ts.count());
    for (const Sample& sample : m_samples) {
        const double t = (static_cast<double>(sample.ts.count()) - origin) / kMillisecondsPerSecond;
        sum_t += t;
        sum_v += sample.speed_mps;
        sum_tt += t * t;
        sum_tv += t * sample.speed_mps;
    }

    const double denominator = count * sum_tt - sum_t * sum_t;
    if (std::fabs(denominator) < 1e-9) {
        return 0.0;
    }
    return (count * sum_tv - sum_t * sum_v) / denominator;
}

void AccelerationEstimator::Clear() {
    m_samples.clear();
    m_smoothed.reset();
    m_last_ts = Timestamp{};
}

} // namespace tmcmt::map
