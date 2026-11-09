#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/st/CameraLinkModel.h"

namespace tmcmt::st {

/// One observed transition between two tracklets.
struct TransitionSample {
    CameraId from;
    CameraId to;
    Timestamp from_end{};
    Timestamp to_begin{};
};

struct EstimatorOptions {
    /// Pairs with fewer valid samples are not emitted.
    std::size_t min_samples = 3;
    /// MAD -> sigma conversion for a normal distribution.
    double mad_scale = 1.4826;
    /// Lower clamp for the estimated sigma, in milliseconds.
    double min_stddev_ms = 1.0;
    /// One-pass trimming: samples farther than this many sigma from the median
    /// are discarded before the final estimate. <= 0 disables trimming.
    double outlier_sigma = 3.0;
    /// Same-camera samples are usually self-matches; skip them by default.
    bool include_same_camera = false;
};

/// Robust transition-time estimator (median / MAD).
///
/// Negative deltas (overlapping tracklets or clock skew) are discarded. The
/// median is used as the location estimate, `sigma = mad_scale * MAD`, with a
/// one-pass 3-sigma trim when enough samples remain.
class TransitionEstimator {
public:
    [[nodiscard]] static Result<std::vector<CameraLink>> Estimate(
        std::span<const TransitionSample> samples, const EstimatorOptions& options = {});
};

} // namespace tmcmt::st
