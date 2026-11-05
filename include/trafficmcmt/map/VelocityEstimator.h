#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

/// Least-squares velocity over a sliding time window, with a finite-difference
/// fallback for very short tracks. Mirrors the smoothing used by
/// Multitarget-tracker's `CTrack::CalcSpeedMps`.
class VelocityEstimator {
public:
    VelocityEstimator(double window_s, double max_mps);

    void Add(Timestamp ts, const EnuPoint& point);

    /// Returns null when the track is too short or the last sample is stale.
    [[nodiscard]] std::optional<EnuPoint> Estimate(Timestamp now) const;

    void Clear();

    [[nodiscard]] std::size_t SampleCount() const noexcept {
        return m_samples.size();
    }

private:
    struct Sample {
        Timestamp ts{};
        EnuPoint point{};
    };

    std::deque<Sample> m_samples;
    double m_window_s = 1.0;
    double m_max_mps = 70.0;
};

} // namespace tmcmt::map
