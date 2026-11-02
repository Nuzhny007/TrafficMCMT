#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/TrackletId.h"

namespace tmcmt {

enum class NormalizeStatus : std::uint8_t {
    Accepted = 0,
    /// The same (session, camera, local_track_id, sequence) was seen before.
    Duplicate = 1,
};

struct NormalizeResult {
    Tracklet tracklet;
    NormalizeStatus status = NormalizeStatus::Accepted;
};

/// Enforces the protocol invariants of docs/architecture.md, section 7.1.
///
/// The normalizer is stateful only for duplicate detection; call `Reset()`
/// between independent sessions. It never throws and never repairs data
/// silently: invalid input produces an `InvalidArgument` / `Unsupported`
/// status with a human-readable explanation.
class TrackletNormalizer {
public:
    struct Options {
        std::uint32_t expected_schema_version = 1;
        /// L2-normalise embeddings whose norm differs from 1.
        bool normalize_embedding = true;
        /// Recompute `quality` from the observation count and embedding norm.
        bool recompute_quality = false;
        /// Upper bound for the duplicate-detection set. When exceeded, the
        /// oldest entries are dropped (the set is a best-effort cache).
        std::size_t max_seen_keys = 1'000'000;
    };

    TrackletNormalizer() = default;
    explicit TrackletNormalizer(Options options);

    [[nodiscard]] Result<NormalizeResult> Normalize(Tracklet input);

    void Reset();

    [[nodiscard]] std::size_t SeenCount() const noexcept {
        return m_seen.size();
    }

private:
    [[nodiscard]] Status Validate(const Tracklet& tracklet) const;
    void Remember(const TrackletKey& key);

    Options m_options;
    std::unordered_set<TrackletKey, TrackletKeyHash> m_seen;
};

} // namespace tmcmt
