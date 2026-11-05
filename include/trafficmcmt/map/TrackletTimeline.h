#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <queue>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

struct ObservationEvent {
    const Tracklet* tracklet = nullptr;
    const Observation* observation = nullptr;
};

/// Merges tracklet chunks into one observation stream ordered by timestamp.
///
/// Chunks are owned by the timeline, so events stay valid until the timeline
/// is destroyed. Memory usage is proportional to the number of tracklets,
/// which is fine for offline replays and bounded live windows.
class TrackletTimeline {
public:
    void Add(Tracklet tracklet);

    [[nodiscard]] bool Empty() const noexcept {
        return m_heap.empty();
    }

    [[nodiscard]] std::size_t TrackletCount() const noexcept {
        return m_tracklets.size();
    }

    [[nodiscard]] std::optional<Timestamp> Begin() const noexcept {
        return m_hasRange ? std::optional<Timestamp>(m_begin) : std::nullopt;
    }

    [[nodiscard]] std::optional<Timestamp> End() const noexcept {
        return m_hasRange ? std::optional<Timestamp>(m_end) : std::nullopt;
    }

    /// Pops the next observation in timestamp order.
    bool Next(ObservationEvent& out);

    /// Timestamp of the next observation without consuming it.
    bool PeekTimestamp(Timestamp& out) const;

    void Clear();

private:
    struct HeapItem {
        Timestamp ts{};
        std::size_t tracklet = 0;
        std::size_t observation = 0;
    };

    struct LaterFirst {
        bool operator()(const HeapItem& lhs, const HeapItem& rhs) const noexcept;
    };

    void PushNext(std::size_t trackletIndex);

    std::vector<Tracklet> m_tracklets;
    std::vector<std::size_t> m_nextObservation;
    std::priority_queue<HeapItem, std::vector<HeapItem>, LaterFirst> m_heap;
    bool m_hasRange = false;
    Timestamp m_begin{};
    Timestamp m_end{};
};

} // namespace tmcmt::map
