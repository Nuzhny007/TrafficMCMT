#include "trafficmcmt/net/SequenceTracker.h"

namespace tmcmt::net {

SequenceTracker::SequenceTracker(std::size_t max_streams)
    : m_max_streams(max_streams == 0 ? 1 : max_streams) {}

std::string SequenceTracker::MakeKey(std::string_view session_id, std::string_view camera_id) {
    std::string key;
    key.reserve(session_id.size() + camera_id.size() + 1);
    key.append(session_id);
    key.push_back('\x1f');
    key.append(camera_id);
    return key;
}

SequenceEvent SequenceTracker::Observe(std::string_view session_id, std::string_view camera_id,
                                       std::uint64_t sequence) {
    std::string key = MakeKey(session_id, camera_id);
    auto [it, inserted] = m_streams.try_emplace(std::move(key));
    if (inserted && m_streams.size() > m_max_streams) {
        // Bound memory for misbehaving senders: forget every stream and start
        // over. The next observation of each stream is reported as `First`.
        m_streams.clear();
        ++m_stats.evictions;
        it = m_streams.try_emplace(MakeKey(session_id, camera_id)).first;
    }

    Stream& stream = it->second;
    ++m_stats.observed;

    if (!stream.has_last) {
        stream.last = sequence;
        stream.has_last = true;
        m_stats.streams = m_streams.size();
        return SequenceEvent{SequenceEventKind::First, 0};
    }

    if (sequence == stream.last) {
        ++m_stats.duplicates;
        return SequenceEvent{SequenceEventKind::Duplicate, 0};
    }
    if (sequence < stream.last) {
        ++m_stats.out_of_order;
        return SequenceEvent{SequenceEventKind::OutOfOrder, 0};
    }
    if (sequence == stream.last + 1) {
        stream.last = sequence;
        return SequenceEvent{SequenceEventKind::InOrder, 0};
    }

    const std::uint64_t missing = sequence - stream.last - 1;
    stream.last = sequence;
    ++m_stats.gaps;
    m_stats.missing_total += missing;
    return SequenceEvent{SequenceEventKind::Gap, missing};
}

void SequenceTracker::Reset() noexcept {
    m_streams.clear();
    m_stats = SequenceStats{};
}

} // namespace tmcmt::net
