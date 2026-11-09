#pragma once

#include <cstdint>
#include <optional>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/TrackletEndpoint.h"

namespace tmcmt::st {

/// The three normalised costs and their weighted total.
struct StCosts {
    /// `c_app = clamp(1 - similarity, 0, 2)`.
    float appearance = 0.f;
    /// `c_time` in [0, 1]; 0 is the most likely transition time.
    float temporal = 0.f;
    /// `c_space` in [0, 1]; 0 means "slow enough", 1 means "impossible".
    float spatial = 0.f;
    /// Weighted mean of the three costs in [0, 2].
    float total = 0.f;
    bool has_transition = false;
    bool has_world = false;
};

struct ScorerOptions {
    StWeights weights{};
    /// Used when the camera pair has no measured transition statistics.
    TransitionWindow window{};
    double max_speed_mps = 30.0;
};

/// Fuses appearance, transition-time and spatial costs into `c_total`.
///
/// Temporal cost:
///   z = (dt - mu) / sigma
///   c_time = clamp(z^2 / 2, 0, 4.5) / 4.5
///
/// This is `-ln(pdf)` normalised by its value at `z = 0` and `z = 3`, so
/// `c_time = 0` at the most likely transition time and `c_time = 1` at the
/// 3-sigma gate. Without measured statistics `mu` and `sigma` are derived from
/// the fallback window: `mu = (min + max) / 2`, `sigma = (max - min) / 6`.
class StReIdScorer {
public:
    explicit StReIdScorer(ScorerOptions options = {});

    [[nodiscard]] StCosts Score(float similarity, const TrackletEndpoint& from,
                                const TrackletEndpoint& to) const;
    [[nodiscard]] StCosts Score(float similarity, const TrackletEndpoint& from,
                                const TrackletEndpoint& to,
                                const std::optional<TransitionStats>& transition) const;

    [[nodiscard]] static float AppearanceCost(float similarity) noexcept;
    [[nodiscard]] static float TemporalCost(std::int64_t delta_ms, const TransitionWindow& window,
                                            const std::optional<TransitionStats>& transition) noexcept;
    [[nodiscard]] static float SpatialCost(std::int64_t delta_ms, const WorldPoint& from_exit,
                                           const WorldPoint& to_entry, double max_speed_mps) noexcept;

    [[nodiscard]] const ScorerOptions& Options() const noexcept {
        return m_options;
    }

private:
    ScorerOptions m_options;
};

} // namespace tmcmt::st
