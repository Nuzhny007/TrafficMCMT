#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Geo.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::bev {

/// One fused world observation. Duplicates are removed before a trajectory is
/// exposed, so every sample here is a kept sample.
struct WorldSample {
    Timestamp ts{};
    EnuPoint position{};
    CameraId camera;
    TrackletId tracklet_id = 0;
    ObjectType type = ObjectType::Unknown;
    float quality = 0.f;
};

/// Fused metric trajectory of one global identity.
struct WorldTrajectory {
    GlobalId global_id = kInvalidGlobalId;
    ObjectType type = ObjectType::Unknown;
    std::vector<WorldSample> samples;

    EnuPoint velocity{};
    bool has_velocity = false;
    double speed_mps = 0.0;
    /// Degrees clockwise from north, [0, 360).
    double course_deg = 0.0;

    Timestamp begin{};
    Timestamp end{};
    bool is_final = false;

    /// WGS-84 origin of the ENU frame the samples live in, when known.
    GeoPoint origin{};
    bool has_origin = false;

    /// Observations removed as duplicates in camera overlap zones.
    std::size_t duplicates_suppressed = 0;
};

struct FusionStats {
    std::uint64_t observations = 0;
    std::uint64_t projected = 0;
    std::uint64_t projection_failures = 0;
    std::uint64_t missing_world = 0;
    std::uint64_t samples_in = 0;
    std::uint64_t samples_out = 0;
    std::uint64_t overlap_pairs = 0;
    std::uint64_t duplicates_suppressed = 0;
};

} // namespace tmcmt::bev
