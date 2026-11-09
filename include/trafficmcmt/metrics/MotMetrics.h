#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::metrics {

/// Ground-truth identity of one tracklet.
struct TrackIdentity {
    TrackletId tracklet_id = 0;
    std::string identity;
    /// Used only to order the tracklets of one identity when counting IDSW.
    Timestamp begin{};
};

/// Predicted global identity of one tracklet.
struct PredictedIdentity {
    TrackletId tracklet_id = 0;
    GlobalId global_id = kInvalidGlobalId;
};

/// Track-level identity metrics.
///
/// Each tracklet is one unit. GT identities and predicted ids are matched with
/// the Hungarian algorithm on the tracklet co-occurrence matrix:
///
///   IDTP = sum of the matched cells
///   IDFP = predicted_tracks - IDTP
///   IDFN = gt_tracks - IDTP
///   IDP  = IDTP / (IDTP + IDFP)
///   IDR  = IDTP / (IDTP + IDFN)
///   IDF1 = 2*IDTP / (2*IDTP + IDFP + IDFN)
///
/// IDSW counts identity switches along each GT trajectory ordered by
/// `TrackIdentity::begin` (ties by tracklet id). Unmatched tracklets are
/// skipped rather than counted as switches.
struct MetricsReport {
    std::size_t gt_tracks = 0;
    std::size_t predicted_tracks = 0;
    std::size_t idtp = 0;
    std::size_t idfp = 0;
    std::size_t idfn = 0;
    std::size_t idsw = 0;
    double idf1 = 0.0;
    double idp = 0.0;
    double idr = 0.0;
};

class MotMetrics {
public:
    [[nodiscard]] static MetricsReport Compute(std::span<const TrackIdentity> gt,
                                               std::span<const PredictedIdentity> predicted);

    /// Minimum-cost assignment for a rectangular matrix. Returns the column
    /// assigned to every row, or `columns` when the row is unmatched.
    [[nodiscard]] static std::vector<std::size_t> Hungarian(
        const std::vector<std::vector<double>>& cost);
};

} // namespace tmcmt::metrics
