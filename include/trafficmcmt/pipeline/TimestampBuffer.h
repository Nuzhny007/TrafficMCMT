#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::pipeline {

struct BufferOptions {
    /// How far behind the newest event a message may arrive.
    std::int64_t max_lateness_ms = 2'000;
    /// A camera that produced nothing for this long is excluded from the
    /// watermark minimum, so one silent camera cannot stall the pipeline.
    std::int64_t silent_timeout_ms = 5'000;
};

struct BufferStats {
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    std::size_t pending = 0;
    std::size_t cameras = 0;
    std::int64_t watermark_ms = 0;
    bool has_watermark = false;
};

/// Event-time buffer that releases items in a deterministic order once the
/// watermark passes them.
///
/// * the watermark is monotone and never moves backwards;
/// * it is `min(active camera max event) - max_lateness`;
/// * cameras silent for longer than `silent_timeout_ms` are ignored;
/// * when every camera is silent the watermark jumps to the newest event;
/// * ties are broken by `(camera, tie_breaker, arrival sequence)`.
template <typename T>
class TimestampBuffer {
public:
    explicit TimestampBuffer(BufferOptions options = {}) : m_options(options) {}

    /// Inserts an item. Admission (late / duplicate / too late) is decided by
    /// `ReplayGuard`; the buffer only orders and releases.
    void Push(T item, Timestamp event_time, const std::string& camera, Timestamp arrived_wall,
              std::uint64_t tie_breaker = 0) {
        ++m_stats.pushed;

        CameraProgress& progress = m_cameras[camera];
        if (progress.max_event < event_time) {
            progress.max_event = event_time;
        }
        progress.last_arrival = arrived_wall;

        const Key key{event_time, camera, tie_breaker, m_sequence++};
        m_items.emplace(key, Entry{std::move(item), event_time, camera});
        RecomputeWatermark(arrived_wall);
    }

    /// Re-evaluates camera liveness. Call on source timeouts.
    void Advance(Timestamp now_wall) {
        RecomputeWatermark(now_wall);
    }

    [[nodiscard]] Timestamp watermark() const noexcept {
        return m_has_watermark ? m_watermark : Timestamp{};
    }

    [[nodiscard]] bool has_watermark() const noexcept {
        return m_has_watermark;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_items.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_items.size();
    }

    /// Removes and returns every item whose event time is `<= watermark`, in
    /// `(event time, camera, tie breaker)` order.
    [[nodiscard]] std::vector<T> PopReady() {
        std::vector<T> out;
        while (!m_items.empty()) {
            auto it = m_items.begin();
            if (m_has_watermark && std::get<0>(it->first) > m_watermark) {
                break;
            }
            out.push_back(std::move(it->second.item));
            m_items.erase(it);
            ++m_stats.popped;
        }
        return out;
    }

    /// Removes and returns everything that is still buffered.
    [[nodiscard]] std::vector<T> DrainAll() {
        std::vector<T> out;
        out.reserve(m_items.size());
        for (auto& [key, entry] : m_items) {
            (void)key;
            out.push_back(std::move(entry.item));
        }
        m_stats.popped += m_items.size();
        m_items.clear();
        return out;
    }

    [[nodiscard]] BufferStats stats() const {
        BufferStats out = m_stats;
        out.pending = m_items.size();
        out.cameras = m_cameras.size();
        out.watermark_ms = m_has_watermark ? m_watermark.count() : 0;
        out.has_watermark = m_has_watermark;
        return out;
    }

    void Reset() {
        m_items.clear();
        m_cameras.clear();
        m_watermark = Timestamp{};
        m_has_watermark = false;
        m_sequence = 0;
        m_stats = BufferStats{};
    }

private:
    using Key = std::tuple<Timestamp, std::string, std::uint64_t, std::uint64_t>;

    struct Entry {
        T item;
        Timestamp event_time;
        std::string camera;
    };

    struct CameraProgress {
        Timestamp max_event{};
        Timestamp last_arrival{};
        bool active = true;
    };

    void RecomputeWatermark(Timestamp now_wall) {
        if (m_cameras.empty()) {
            return;
        }

        Timestamp min_active = Timestamp::max();
        Timestamp max_seen = Timestamp::min();
        bool any_active = false;
        for (auto& [camera, progress] : m_cameras) {
            (void)camera;
            const Timestamp idle = now_wall - progress.last_arrival;
            progress.active = idle <= Timestamp(m_options.silent_timeout_ms);
            if (progress.active) {
                any_active = true;
                min_active = std::min(min_active, progress.max_event);
            }
            max_seen = std::max(max_seen, progress.max_event);
        }

        const Timestamp candidate =
            any_active ? (min_active - Timestamp(m_options.max_lateness_ms)) : max_seen;
        if (!m_has_watermark || candidate > m_watermark) {
            m_watermark = candidate;
            m_has_watermark = true;
        }
    }

    BufferOptions m_options;
    std::map<Key, Entry> m_items;
    std::unordered_map<std::string, CameraProgress> m_cameras;
    Timestamp m_watermark{};
    bool m_has_watermark = false;
    std::uint64_t m_sequence = 0;
    BufferStats m_stats;
};

} // namespace tmcmt::pipeline
