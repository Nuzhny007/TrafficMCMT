#pragma once

#include <cstdint>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

struct TrackingObject;

namespace tmcmt {

struct MttAdapterOptions {
    /// Minimum detector confidence. Negative confidences (motion detectors)
    /// are treated as "unknown" and always pass.
    float min_confidence = 0.f;
    bool skip_static = true;
    bool skip_out_of_frame = false;
    /// Minimum MTT trajectory length before a track is exported.
    int min_trace_length = 2;
};

/// Converts Multitarget-tracker objects into protocol observations.
///
/// This is the only place in the project that knows about `TrackingObject`.
/// `TrackingObject` does not carry a timestamp, so the caller supplies the
/// capture timestamp explicitly.
class MttTrackAdapter {
public:
    explicit MttTrackAdapter(MttAdapterOptions options = {});

    [[nodiscard]] bool ShouldExport(const TrackingObject& track) const;

    [[nodiscard]] Result<Observation> ToObservation(const TrackingObject& track, Timestamp ts) const;

    [[nodiscard]] static BBox ToBBox(const TrackingObject& track) noexcept;

    /// Maps MTT object types (`TypeConverter::Type2Str`) to the protocol enum.
    [[nodiscard]] static ObjectType ToObjectType(int mtt_type) noexcept;

private:
    MttAdapterOptions m_options;
};

} // namespace tmcmt
