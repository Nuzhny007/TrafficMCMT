#include "trafficmcmt/st/StReIdScorer.h"

#include <algorithm>
#include <cmath>

namespace tmcmt::st {
namespace {

constexpr double kEpsilon = 1e-9;
/// Value of `z^2 / 2` at the 3-sigma gate.
constexpr double kThreeSigmaCost = 4.5;

} // namespace

StReIdScorer::StReIdScorer(ScorerOptions options) : m_options(options) {}

float StReIdScorer::AppearanceCost(float similarity) noexcept {
    return std::clamp(1.f - similarity, 0.f, 2.f);
}

float StReIdScorer::TemporalCost(std::int64_t delta_ms, const TransitionWindow& window,
                                 const std::optional<TransitionStats>& transition) noexcept {
    double mean = 0.0;
    double sigma = 0.0;

    if (transition && transition->IsValid()) {
        mean = transition->mean_ms;
        sigma = transition->stddev_ms;
    } else {
        if (!window.IsValid()) {
            return 0.f;
        }
        mean = 0.5 * (static_cast<double>(window.min_ms) + static_cast<double>(window.max_ms));
        sigma = (static_cast<double>(window.max_ms) - static_cast<double>(window.min_ms)) / 6.0;
    }
    if (!(sigma > 0.0)) {
        return 0.f;
    }

    const double z = (static_cast<double>(delta_ms) - mean) / sigma;
    // `-ln(pdf)` differs from `z^2 / 2` only by a constant; subtracting it
    // makes the cost zero at the mean and one at the 3-sigma gate.
    return static_cast<float>(std::clamp(0.5 * z * z / kThreeSigmaCost, 0.0, 1.0));
}

float StReIdScorer::SpatialCost(std::int64_t delta_ms, const WorldPoint& from_exit,
                                const WorldPoint& to_entry, double max_speed_mps) noexcept {
    if (!from_exit.IsFinite() || !to_entry.IsFinite()) {
        return 1.f;
    }
    const double dx = static_cast<double>(from_exit.x) - static_cast<double>(to_entry.x);
    const double dy = static_cast<double>(from_exit.y) - static_cast<double>(to_entry.y);
    const double distance = std::hypot(dx, dy);

    if (!(max_speed_mps > 0.0)) {
        return distance > kEpsilon ? 1.f : 0.f;
    }
    const double deltaSeconds = static_cast<double>(delta_ms) / 1000.0;
    if (!(deltaSeconds > 0.0)) {
        return distance > kEpsilon ? 1.f : 0.f;
    }
    return static_cast<float>(std::clamp(distance / (max_speed_mps * deltaSeconds), 0.0, 1.0));
}

StCosts StReIdScorer::Score(float similarity, const TrackletEndpoint& from,
                            const TrackletEndpoint& to) const {
    return Score(similarity, from, to, std::nullopt);
}

StCosts StReIdScorer::Score(float similarity, const TrackletEndpoint& from,
                            const TrackletEndpoint& to,
                            const std::optional<TransitionStats>& transition) const {
    StCosts costs;
    costs.appearance = AppearanceCost(similarity);

    const std::int64_t deltaMs = (to.begin - from.end).count();
    costs.temporal = TemporalCost(deltaMs, m_options.window, transition);
    costs.has_transition = transition.has_value() && transition->IsValid();

    costs.has_world = from.HasExit() && to.HasEntry();
    costs.spatial = SpatialCost(deltaMs, from.exit, to.entry, m_options.max_speed_mps);

    const double weightSum = m_options.weights.appearance + m_options.weights.temporal +
                             m_options.weights.spatial;
    if (!(weightSum > 0.0)) {
        costs.total = costs.appearance;
        return costs;
    }
    const double total = (m_options.weights.appearance * costs.appearance +
                          m_options.weights.temporal * costs.temporal +
                          m_options.weights.spatial * costs.spatial) /
                         weightSum;
    costs.total = static_cast<float>(std::clamp(total, 0.0, 2.0));
    return costs;
}

} // namespace tmcmt::st
