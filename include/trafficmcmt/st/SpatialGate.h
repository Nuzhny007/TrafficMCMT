#pragma once

#include <cstdint>
#include <optional>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/st/CameraLinkModel.h"
#include "trafficmcmt/st/TrackletEndpoint.h"

namespace tmcmt::st {

/// Why a pair was accepted or rejected by `SpatialGate`.
enum class GateReason : std::uint8_t {
    Allowed = 0,
    SameCamera,
    Unlinked,
    OutsideWindow,
    OutsideSigma,
    MissingWorld,
    MissingZone,
    ExitZone,
    EntryZone,
    Speed,
    Distance,
};

const char* ToString(GateReason reason) noexcept;

struct GateDecision {
    bool allowed = false;
    GateReason reason = GateReason::Allowed;
};

struct SpatialGateOptions {
    GateMode mode = GateMode::Off;
    /// Fallback transition window; used when the model has no measured
    /// statistics for a pair.
    TransitionWindow window{};
    double max_speed_mps = 30.0;
    double max_distance_m = 200.0;
    /// Debug switch: when true, missing world coordinates skip zone/BEV checks
    /// instead of rejecting the pair.
    bool allow_missing_world = false;
    /// st-ReID is cross-camera by definition; same-camera pairs are rejected.
    bool reject_same_camera = true;
};

/// Applies topology, temporal, zone and BEV gates to an ordered pair.
///
/// `model` may be null: the link and measured-transition checks are skipped,
/// while the fallback window, zone and BEV checks still apply.
class SpatialGate {
public:
    SpatialGate(const CameraLinkModel* model, SpatialGateOptions options);

    [[nodiscard]] GateDecision Check(const TrackletEndpoint& from, const TrackletEndpoint& to) const;

    [[nodiscard]] const SpatialGateOptions& Options() const noexcept {
        return m_options;
    }
    [[nodiscard]] const CameraLinkModel* Model() const noexcept {
        return m_model;
    }

private:
    [[nodiscard]] std::optional<TransitionStats> ResolveTransition(const CameraId& from,
                                                                   const CameraId& to) const;

    const CameraLinkModel* m_model = nullptr;
    SpatialGateOptions m_options;
};

} // namespace tmcmt::st
