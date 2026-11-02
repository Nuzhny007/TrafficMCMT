#include "trafficmcmt/io/TrackletNormalizer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tmcmt {
namespace {

Status Invalid(const std::string& what) {
    return Status::Error(ErrorCode::InvalidArgument, what);
}

bool IsSortedByTimestamp(const std::vector<Observation>& observations) {
    for (std::size_t i = 1; i < observations.size(); ++i) {
        if (observations[i].ts < observations[i - 1].ts) {
            return false;
        }
    }
    return true;
}

} // namespace

TrackletNormalizer::TrackletNormalizer(Options options) : m_options(std::move(options)) {}

void TrackletNormalizer::Reset() {
    m_seen.clear();
}

Status TrackletNormalizer::Validate(const Tracklet& tracklet) const {
    if (tracklet.schema_version != m_options.expected_schema_version) {
        return Status::Error(ErrorCode::Unsupported,
                             "unsupported schema_version " + std::to_string(tracklet.schema_version) + ", expected " +
                                 std::to_string(m_options.expected_schema_version));
    }
    if (tracklet.session_id.empty()) {
        return Invalid("session_id must not be empty");
    }
    if (tracklet.camera.empty()) {
        return Invalid("camera_id must not be empty");
    }
    if (tracklet.end < tracklet.begin) {
        return Invalid("begin must be <= end");
    }
    if (tracklet.observations.empty()) {
        return Invalid("observations must not be empty");
    }
    for (const Observation& observation : tracklet.observations) {
        if (!observation.bbox.IsFinite()) {
            return Invalid("observation bbox contains a non-finite value");
        }
        if (observation.bbox.w < 0.f || observation.bbox.h < 0.f) {
            return Invalid("observation bbox width and height must be >= 0");
        }
        if (observation.world.valid && !observation.world.IsFinite()) {
            return Invalid("observation world point contains a non-finite value");
        }
    }

    if (!tracklet.embedding.IsValid()) {
        return Invalid("embedding dim does not match the number of values");
    }
    for (const float value : tracklet.embedding.values) {
        if (!std::isfinite(value)) {
            return Invalid("embedding contains a non-finite value");
        }
    }
    if (!(tracklet.embedding.Norm() > 0.f)) {
        return Invalid("embedding norm must be > 0");
    }
    if (!std::isfinite(tracklet.quality)) {
        return Invalid("quality must be finite");
    }
    return Status::Ok();
}

void TrackletNormalizer::Remember(const TrackletKey& key) {
    if (m_options.max_seen_keys > 0 && m_seen.size() >= m_options.max_seen_keys) {
        m_seen.clear();
    }
    m_seen.insert(key);
}

Result<NormalizeResult> TrackletNormalizer::Normalize(Tracklet input) {
    Status status = Validate(input);
    if (!status.ok()) {
        return status;
    }

    if (!IsSortedByTimestamp(input.observations)) {
        std::stable_sort(input.observations.begin(), input.observations.end(),
                         [](const Observation& lhs, const Observation& rhs) { return lhs.ts < rhs.ts; });
    }

    // The declared interval must cover every observation.
    input.begin = std::min(input.begin, input.observations.front().ts);
    input.end = std::max(input.end, input.observations.back().ts);

    if (m_options.normalize_embedding) {
        const float norm = input.embedding.Norm();
        if (norm > 0.f && std::fabs(norm - 1.f) > 1e-6f) {
            const float inv = 1.f / norm;
            for (float& value : input.embedding.values) {
                value *= inv;
            }
        }
    }

    input.quality = std::clamp(input.quality, 0.f, 1.f);
    if (m_options.recompute_quality) {
        const float density = std::min(1.f, static_cast<float>(input.observations.size()) / 20.f);
        input.quality = std::clamp(density * input.quality, 0.f, 1.f);
    }

    AssignTrackletId(input);

    const TrackletKey key = MakeTrackletKey(input);
    if (m_seen.find(key) != m_seen.end()) {
        return NormalizeResult{std::move(input), NormalizeStatus::Duplicate};
    }
    Remember(key);
    return NormalizeResult{std::move(input), NormalizeStatus::Accepted};
}

} // namespace tmcmt
