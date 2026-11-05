#include "trafficmcmt/map/TrackletTimeline.h"

#include <utility>

namespace tmcmt::map {

bool TrackletTimeline::LaterFirst::operator()(const HeapItem& lhs, const HeapItem& rhs) const noexcept {
    if (lhs.ts != rhs.ts) {
        return lhs.ts > rhs.ts;
    }
    if (lhs.tracklet != rhs.tracklet) {
        return lhs.tracklet > rhs.tracklet;
    }
    return lhs.observation > rhs.observation;
}

void TrackletTimeline::Add(Tracklet tracklet) {
    if (tracklet.observations.empty()) {
        return;
    }

    for (const Observation& observation : tracklet.observations) {
        if (!m_hasRange) {
            m_begin = observation.ts;
            m_end = observation.ts;
            m_hasRange = true;
        } else {
            if (observation.ts < m_begin) {
                m_begin = observation.ts;
            }
            if (observation.ts > m_end) {
                m_end = observation.ts;
            }
        }
    }

    const std::size_t index = m_tracklets.size();
    m_tracklets.push_back(std::move(tracklet));
    m_nextObservation.push_back(0);
    PushNext(index);
}

bool TrackletTimeline::Next(ObservationEvent& out) {
    if (m_heap.empty()) {
        return false;
    }

    const HeapItem item = m_heap.top();
    m_heap.pop();

    const Tracklet& tracklet = m_tracklets[item.tracklet];
    out.tracklet = &tracklet;
    out.observation = &tracklet.observations[item.observation];

    m_nextObservation[item.tracklet] = item.observation + 1;
    PushNext(item.tracklet);
    return true;
}

bool TrackletTimeline::PeekTimestamp(Timestamp& out) const {
    if (m_heap.empty()) {
        return false;
    }
    out = m_heap.top().ts;
    return true;
}

void TrackletTimeline::PushNext(std::size_t trackletIndex) {    const Tracklet& tracklet = m_tracklets[trackletIndex];
    const std::size_t next = m_nextObservation[trackletIndex];
    if (next < tracklet.observations.size()) {
        m_heap.push(HeapItem{tracklet.observations[next].ts, trackletIndex, next});
    }
}

void TrackletTimeline::Clear() {
    m_tracklets.clear();
    m_nextObservation.clear();
    m_heap = {};
    m_hasRange = false;
    m_begin = Timestamp{};
    m_end = Timestamp{};
}

} // namespace tmcmt::map
