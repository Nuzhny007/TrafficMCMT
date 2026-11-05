#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "trafficmcmt/core/Types.h"

namespace tmcmt::map {

/// Identity of one Edge tracklet stream.
struct TrackKey {
    std::string session_id;
    CameraId camera;
    std::uint64_t local_track_id = 0;

    [[nodiscard]] bool operator==(const TrackKey& other) const noexcept {
        return local_track_id == other.local_track_id && camera == other.camera &&
               session_id == other.session_id;
    }
};

struct TrackKeyHash {
    [[nodiscard]] std::size_t operator()(const TrackKey& key) const noexcept;
};

/// Assigns small, stable, JavaScript-safe ids to Edge tracklet keys.
///
/// Ids are allocated sequentially in first-appearance order. This keeps the
/// stream deterministic for a given input and avoids the precision loss of
/// exposing 64-bit tracklet hashes to the browser.
class MapIdRegistry {
public:
    explicit MapIdRegistry(std::uint64_t first_id = 1);

    /// Returns the existing id or allocates a new one.
    [[nodiscard]] std::uint64_t Assign(const TrackKey& key);

    [[nodiscard]] std::optional<std::uint64_t> Find(const TrackKey& key) const;

    /// Re-keys an existing track, e.g. after the global solver merges two
    /// identities. Returns false when `old_id` is unknown or `new_id` is taken.
    bool Rename(std::uint64_t old_id, std::uint64_t new_id);

    [[nodiscard]] std::size_t Size() const noexcept {
        return m_ids.size();
    }

    [[nodiscard]] std::uint64_t NextId() const noexcept {
        return m_next;
    }

private:
    std::unordered_map<TrackKey, std::uint64_t, TrackKeyHash> m_ids;
    std::uint64_t m_next = 1;
};

} // namespace tmcmt::map
