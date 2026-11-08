#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/reid/GalleryManager.h"

namespace tmcmt::reid {

/// One appearance hypothesis for a query tracklet.
struct MatchCandidate {
    TrackletId query_id = 0;
    TrackletId candidate_id = 0;
    /// Cosine similarity in [-1, 1] (embeddings are expected to be L2-normalised).
    float similarity = 0.f;
    /// Appearance cost `c_app = 1 - similarity`, clamped to [0, 2].
    float cost = 0.f;
};

struct MatcherOptions {
    std::size_t top_k = 20;
    double min_similarity = 0.0;
    bool exclude_same_camera = true;
};

/// Turns gallery search results into appearance candidates for the
/// spatio-temporal stage. Descriptors from different ReID models are never
/// compared: the matcher restricts the search to the query's `model_id`.
class ReIdMatcher {
public:
    ReIdMatcher(const GalleryManager& gallery, MatcherOptions options);

    [[nodiscard]] Result<std::vector<MatchCandidate>> Match(const Tracklet& query) const;

    [[nodiscard]] Result<std::vector<MatchCandidate>> Match(TrackletId query_id, const CameraId& camera,
                                                            std::span<const float> values,
                                                            std::uint32_t model_id) const;

private:
    const GalleryManager& m_gallery;
    MatcherOptions m_options;
};

} // namespace tmcmt::reid
