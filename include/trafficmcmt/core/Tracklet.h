#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// Axis-aligned bounding box in image coordinates (pixels).
struct BBox {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    float score = 0.f;

    [[nodiscard]] float Area() const noexcept {
        return (w > 0.f && h > 0.f) ? w * h : 0.f;
    }

    [[nodiscard]] bool IsFinite() const noexcept {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
               std::isfinite(score);
    }
};

/// Ground-plane position in meters (BEV).
struct WorldPoint {
    float x = 0.f;
    float y = 0.f;
    bool valid = false;

    [[nodiscard]] bool IsFinite() const noexcept {
        return valid && std::isfinite(x) && std::isfinite(y);
    }
};

/// One tracked position inside a tracklet.
struct Observation {
    Timestamp ts{};
    BBox bbox{};
    WorldPoint world{};
};

/// Aggregated appearance descriptor of a tracklet.
struct Embedding {
    std::vector<float> values;
    std::uint32_t model_id = 0;
    std::uint16_t dim = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return dim > 0 && values.size() == static_cast<std::size_t>(dim);
    }

    [[nodiscard]] float Norm() const noexcept {
        float sum = 0.f;
        for (const float value : values) {
            sum += value * value;
        }
        return std::sqrt(sum);
    }
};

/// A fragment of a single-camera trajectory with an aggregated embedding.
/// This is the unit of exchange between Edge nodes and the Master node.
///
/// Identity: `session_id` + `camera` + `local_track_id` + `sequence`.
/// The derived `id` is a stable 64-bit hash of that tuple and is used as a
/// map key inside the Master node.
struct Tracklet {
    TrackletId id = 0;

    /// UUID of the Edge run that produced the tracklet.
    std::string session_id;
    CameraId camera;

    /// Tracker-local id, unique within (session_id, camera).
    std::uint64_t local_track_id = 0;

    /// Monotonically increasing chunk counter within (session_id, camera,
    /// local_track_id). Starts at 0.
    std::uint64_t sequence = 0;

    /// True for the final chunk of a track (`MESSAGE_KIND_TRACK_END`).
    bool is_end = false;

    Timestamp begin{};
    Timestamp end{};
    std::vector<Observation> observations;
    Embedding embedding;
    ObjectType type = ObjectType::Unknown;
    float quality = 1.f;
    std::uint32_t schema_version = 1;

    [[nodiscard]] std::int64_t DurationMs() const noexcept {
        return (end - begin).count();
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !session_id.empty() && !camera.empty() && end >= begin && !observations.empty() &&
               embedding.IsValid() && std::isfinite(quality);
    }
};

} // namespace tmcmt
