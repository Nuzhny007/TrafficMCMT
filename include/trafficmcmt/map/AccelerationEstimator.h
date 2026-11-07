#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

/// Least-squares acceleration over a sliding window of smoothed speed samples.
///
/// The estimate is clamped and exponentially smoothed because HBEFA is very
/// sensitive to acceleration: a noisy derivative would make the emission field
/// flicker. Returns nullopt until at least two samples are available or when
/// the track has been silent for more than a second.
class AccelerationEstimator {
public:
    explicit AccelerationEstimator(double window_s = 1.5, double max_mps2 = 5.0, double smoothing = 0.3);

    void Add(Timestamp ts, double speed_mps);

    [[nodiscard]] std::optional<double> Estimate(Timestamp now) const;

    void Clear();

    [[nodiscard]] std::size_t SampleCount() const noexcept {
        return m_samples.size();
    }

private:
    struct Sample {
        Timestamp ts{};
        double speed_mps = 0.0;
    };

    [[nodiscard]] double FitSlope() const noexcept;

    std::deque<Sample> m_samples;
    double m_window_s = 1.5;
    double m_max_mps2 = 5.0;
    double m_smoothing = 0.3;
    std::optional<double> m_smoothed;
    Timestamp m_last_ts{};
};

} // namespace tmcmt::map
