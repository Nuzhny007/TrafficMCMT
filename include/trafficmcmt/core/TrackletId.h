#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// Logical identity of a tracklet chunk.
///
/// Two chunks belong to the same physical track when (session_id, camera,
/// local_track_id) match; `sequence` distinguishes successive chunks of that
/// track.
struct TrackletKey {
    std::string session_id;
    CameraId camera;
    std::uint64_t local_track_id = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool operator==(const TrackletKey& other) const noexcept {
        return session_id == other.session_id && camera == other.camera &&
               local_track_id == other.local_track_id && sequence == other.sequence;
    }

    [[nodiscard]] auto Tie() const noexcept {
        return std::tie(session_id, camera, local_track_id, sequence);
    }

    [[nodiscard]] bool operator<(const TrackletKey& other) const noexcept {
        return Tie() < other.Tie();
    }
};

struct TrackletKeyHash {
    [[nodiscard]] std::size_t operator()(const TrackletKey& key) const noexcept;
};

/// Returns the key of a tracklet.
[[nodiscard]] TrackletKey MakeTrackletKey(const Tracklet& tracklet);

/// Derives a stable non-zero 64-bit id from (session_id, camera,
/// local_track_id, sequence) using FNV-1a.
[[nodiscard]] TrackletId MakeTrackletId(std::string_view session_id, std::string_view camera,
                                        std::uint64_t local_track_id, std::uint64_t sequence) noexcept;

/// Fills `tracklet.id` from its identity fields.
void AssignTrackletId(Tracklet& tracklet) noexcept;

} // namespace tmcmt
