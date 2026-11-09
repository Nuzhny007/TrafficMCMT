#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/reid/GalleryManager.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/SpatialGate.h"
#include "trafficmcmt/st/StReIdScorer.h"
#include "trafficmcmt/st/TrackletEndpoint.h"

namespace tmcmt::st {

/// One accepted cross-camera hypothesis.
struct PairCandidate {
    TrackletId query_id = 0;
    TrackletId candidate_id = 0;
    CameraId from_camera;
    CameraId to_camera;
    float similarity = 0.f;
    StCosts costs;
    GateReason gate = GateReason::Allowed;
};

/// Aggregate counters useful for reports and for tuning the gates.
struct PairGenerationStats {
    std::size_t considered = 0;
    std::size_t appearance_candidates = 0;
    std::size_t rejected_same_camera = 0;
    std::size_t rejected_unlinked = 0;
    std::size_t rejected_window = 0;
    std::size_t rejected_sigma = 0;
    std::size_t rejected_missing_world = 0;
    std::size_t rejected_missing_zone = 0;
    std::size_t rejected_exit_zone = 0;
    std::size_t rejected_entry_zone = 0;
    std::size_t rejected_speed = 0;
    std::size_t rejected_distance = 0;
    std::size_t rejected_cost = 0;
    std::size_t truncated = 0;
    std::size_t accepted = 0;
};

struct PairGenerationOptions {
    std::size_t top_k = 20;
    double min_similarity = 0.0;
    /// Appearance candidates fetched per query before the st gates. The
    /// effective ANN `top_k` is `top_k * oversample`.
    std::size_t oversample = 4;
    bool exclude_same_camera = true;
    /// Reject pairs whose fused cost exceeds this value.
    double max_total_cost = 1.0;

    StWeights weights{};
    TransitionWindow window{};
    GateMode gate_mode = GateMode::Off;
    double max_speed_mps = 30.0;
    double max_distance_m = 200.0;
    bool allow_missing_world = false;
};

/// Generates appearance candidates, applies the st gates and ranks the
/// survivors by ascending `c_total`.
class PairCandidateGenerator {
public:
    PairCandidateGenerator(const reid::GalleryManager& gallery, const CameraLinkModel* model,
                           PairGenerationOptions options);

    [[nodiscard]] Result<std::vector<PairCandidate>> Generate(const Tracklet& query);
    [[nodiscard]] Result<std::vector<PairCandidate>> Generate(std::span<const Tracklet> queries);

    [[nodiscard]] const PairGenerationStats& Stats() const noexcept {
        return m_stats;
    }

private:
    [[nodiscard]] Result<std::vector<PairCandidate>> GenerateOne(const Tracklet& query,
                                                                PairGenerationStats& stats) const;

    const reid::GalleryManager& m_gallery;
    const CameraLinkModel* m_model = nullptr;
    PairGenerationOptions m_options;
    PairGenerationStats m_stats;
};

} // namespace tmcmt::st
