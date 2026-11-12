#pragma once

#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/SpatialGate.h"
#include "trafficmcmt/st/TrackletEndpoint.h"

namespace tmcmt::st {

/// Spatial gate that prefers metric (BEV) checks and falls back to the
/// zone-based checks when a pair has no world endpoints.
///
/// This is intentionally a thin wrapper: the actual distance, speed, polygon
/// and temporal logic stays in `SpatialGate`. The wrapper only chooses which
/// `SpatialGate` instance to run, so the two modes cannot drift apart.
class BeVSpatialGate {
public:
    /// `options.mode` selects the preferred mode. With `fallback_to_zone` the
    /// gate switches to `GateMode::Zone` for pairs that have no world
    /// endpoints; otherwise those pairs are handled by the BEV gate (and
    /// rejected as `missing_world` unless `allow_missing_world` is set).
    BeVSpatialGate(const CameraLinkModel* model, SpatialGateOptions options,
                   bool fallback_to_zone = true);

    [[nodiscard]] GateDecision Check(const TrackletEndpoint& from,
                                     const TrackletEndpoint& to) const;

    [[nodiscard]] const SpatialGate& Preferred() const noexcept {
        return m_preferred;
    }
    [[nodiscard]] const SpatialGate& Zone() const noexcept {
        return m_zone;
    }
    [[nodiscard]] bool FallsBackToZone() const noexcept {
        return m_fallbackToZone;
    }

private:
    SpatialGate m_preferred;
    SpatialGate m_zone;
    bool m_fallbackToZone = true;
};

} // namespace tmcmt::st
