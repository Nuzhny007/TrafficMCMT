#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "trafficmcmt/bev/HomographyProjector.h"
#include "trafficmcmt/bev/WorldTypes.h"
#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::bev {

struct WorldFusionOptions {
    bool enabled = false;
    bool overwrite_world = false;
    double duplicate_distance_m = 1.5;
    int duplicate_window_ms = 500;
    int speed_window_ms = 1000;
    int smooth_window = 0;
    GeoPoint shared_origin{};
    bool has_shared_origin = false;
};

/// Projects missing world points, normalises every camera to one ENU frame,
/// de-duplicates observations in overlap zones and derives speed/course.
///
/// The class is deterministic: fusion sorts by `(timestamp, camera, tracklet)`
/// and breaks quality ties by tracklet id, so shuffling the input segments
/// produces the same trajectory.
class WorldTrajectoryFusion {
public:
    explicit WorldTrajectoryFusion(WorldFusionOptions options = {});

    /// Fills `Observation.world` where it is missing (or everywhere when
    /// `overwrite_world` is set) and translates existing world points into the
    /// shared ENU origin. Projection failures are counted, never fatal.
    [[nodiscard]] Status Normalize(Tracklet& tracklet, const CameraCalibration* calibration,
                                   const GeoPoint& fallback_origin = {});

    /// Fuses all segments of a global update into one metric trajectory.
    [[nodiscard]] WorldTrajectory Fuse(const GlobalTrackUpdate& update) const;

    [[nodiscard]] FusionStats Stats() const noexcept;
    void ResetStats() noexcept;

    [[nodiscard]] const WorldFusionOptions& Options() const noexcept {
        return m_options;
    }

private:
    struct ProjectorCache {
        std::shared_ptr<const CameraCalibration> calibration;
        std::optional<HomographyProjector> projector;
    };

    [[nodiscard]] const HomographyProjector* ProjectorFor(const CameraCalibration& calibration);
    [[nodiscard]] GeoPoint ResolveTargetOrigin(const Tracklet& tracklet,
                                               const CameraCalibration* calibration,
                                               const GeoPoint& fallback) const;
    [[nodiscard]] GeoPoint ResolveSourceOrigin(const Tracklet& tracklet,
                                               const CameraCalibration* calibration,
                                               const GeoPoint& fallback) const;

    WorldFusionOptions m_options;
    std::map<CameraId, ProjectorCache> m_projectors;
    mutable FusionStats m_stats;
};

} // namespace tmcmt::bev
