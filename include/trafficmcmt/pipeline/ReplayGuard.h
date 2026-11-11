#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/pipeline/WindowScheduler.h"

namespace tmcmt::pipeline {

enum class ArrivalClass : std::uint8_t {
    /// Arrived before the watermark, or its window has not been solved yet.
    InOrder = 0,
    /// Arrived after the watermark but its window can still be reopened.
    LateWithinWindow = 1,
    /// Arrived after the watermark and the window is already committed.
    TooLate = 2,
    /// The same tracklet was already accepted.
    Duplicate = 3,
};

struct ReplayGuardOptions {
    /// Bounded duplicate-detection cache. Oldest ids are dropped first.
    std::size_t max_seen_keys = 10'000;
};

struct ReplayGuardStats {
    std::uint64_t in_order = 0;
    std::uint64_t late = 0;
    std::uint64_t too_late = 0;
    std::uint64_t duplicates = 0;
};

/// Classifies arrivals and suppresses duplicates.
///
/// A tracklet id is the stable FNV hash of
/// `(session_id, camera, local_track_id, sequence)`, so the guard also protects
/// against a replayed file or a reconnecting publisher.
class ReplayGuard {
public:
    explicit ReplayGuard(ReplayGuardOptions options = {});

    /// Classifies and remembers an arrival.
    [[nodiscard]] ArrivalClass Classify(TrackletId id, Timestamp event_time, Timestamp watermark,
                                        bool has_watermark, WindowState window_state);

    [[nodiscard]] const ReplayGuardStats& stats() const noexcept {
        return m_stats;
    }

    void Reset();

private:
    /// Returns `true` when the id was not seen before.
    bool Remember(TrackletId id);

    ReplayGuardOptions m_options;
    std::unordered_set<TrackletId> m_seen;
    std::deque<TrackletId> m_order;
    ReplayGuardStats m_stats;
};

} // namespace tmcmt::pipeline
