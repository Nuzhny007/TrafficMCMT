#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tmcmt::net {

enum class SequenceEventKind : std::uint8_t {
    First = 0,
    InOrder,
    Gap,
    Duplicate,
    OutOfOrder,
};

struct SequenceEvent {
    SequenceEventKind kind = SequenceEventKind::First;
    /// Number of skipped sequence numbers for `Gap`; zero otherwise.
    std::uint64_t missing = 0;
};

struct SequenceStats {
    std::uint64_t observed = 0;
    std::uint64_t gaps = 0;
    std::uint64_t missing_total = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t streams = 0;
    std::uint64_t evictions = 0;
};

/// Tracks monotonic `sequence` numbers per `(session_id, camera_id)` stream.
///
/// `sequence` is assigned by the Edge node and grows by one per tracklet chunk.
/// A gap means that a message was lost in transit; the tracker never tries to
/// reorder, it only reports what it observed.
class SequenceTracker {
public:
    explicit SequenceTracker(std::size_t max_streams = 4096);

    [[nodiscard]] SequenceEvent Observe(std::string_view session_id, std::string_view camera_id,
                                        std::uint64_t sequence);

    [[nodiscard]] const SequenceStats& stats() const noexcept {
        return m_stats;
    }

    void Reset() noexcept;

    [[nodiscard]] static std::string MakeKey(std::string_view session_id, std::string_view camera_id);

private:
    struct Stream {
        std::uint64_t last = 0;
        bool has_last = false;
    };

    std::unordered_map<std::string, Stream> m_streams;
    std::size_t m_max_streams;
    SequenceStats m_stats;
};

} // namespace tmcmt::net
