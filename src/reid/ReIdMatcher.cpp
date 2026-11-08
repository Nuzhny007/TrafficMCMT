#include "trafficmcmt/reid/ReIdMatcher.h"

#include <algorithm>
#include <utility>

namespace tmcmt::reid {
namespace {

float AppearanceCost(float similarity) noexcept {
    return std::clamp(1.f - similarity, 0.f, 2.f);
}

} // namespace

ReIdMatcher::ReIdMatcher(const GalleryManager& gallery, MatcherOptions options)
    : m_gallery(gallery), m_options(options) {}

Result<std::vector<MatchCandidate>> ReIdMatcher::Match(const Tracklet& query) const {
    if (!query.embedding.IsValid()) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "query tracklet " + std::to_string(query.id) + " has no valid embedding");
    }
    return Match(query.id, query.camera,
                 std::span<const float>(query.embedding.values.data(), query.embedding.values.size()),
                 query.embedding.model_id);
}

Result<std::vector<MatchCandidate>> ReIdMatcher::Match(TrackletId query_id, const CameraId& camera,
                                                       std::span<const float> values,
                                                       std::uint32_t model_id) const {
    GalleryQuery query;
    query.top_k = m_options.top_k;
    query.query_camera = camera;
    query.exclude_same_camera = m_options.exclude_same_camera;
    query.exclude_id = query_id;
    query.model_id = model_id;
    query.min_similarity = static_cast<float>(m_options.min_similarity);

    auto hits = m_gallery.Search(values, query);
    if (!hits) {
        return hits.status();
    }

    std::vector<MatchCandidate> candidates;
    candidates.reserve(hits.value().size());
    for (const SearchHit& hit : hits.value()) {
        candidates.push_back(MatchCandidate{query_id, hit.id, hit.score, AppearanceCost(hit.score)});
    }
    return candidates;
}

} // namespace tmcmt::reid
