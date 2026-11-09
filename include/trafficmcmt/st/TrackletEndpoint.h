#pragma once

#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::reid {
struct GalleryEntry;
} // namespace tmcmt::reid

namespace tmcmt::st {

/// The two ends of a tracklet used by the spatio-temporal gates.
///
/// `entry` is the first observation with valid world coordinates, `exit` is the
/// last one. Both are invalid when the tracklet has no geo binding.
struct TrackletEndpoint {
    TrackletId id = 0;
    CameraId camera;
    ObjectType type = ObjectType::Unknown;
    Timestamp begin{};
    Timestamp end{};
    WorldPoint entry{};
    WorldPoint exit{};

    [[nodiscard]] bool HasEntry() const noexcept {
        return entry.IsFinite();
    }
    [[nodiscard]] bool HasExit() const noexcept {
        return exit.IsFinite();
    }
    [[nodiscard]] bool HasWorld() const noexcept {
        return HasEntry() && HasExit();
    }
};

/// Extracts endpoints from a raw tracklet.
[[nodiscard]] TrackletEndpoint MakeEndpoint(const Tracklet& tracklet);

/// Extracts endpoints from a gallery entry. `GalleryManager::Add(Tracklet)`
/// fills `entry_world` / `exit_world`; entries added as `GalleryEntry` may
/// leave them invalid.
[[nodiscard]] TrackletEndpoint MakeEndpoint(const reid::GalleryEntry& entry);

} // namespace tmcmt::st
