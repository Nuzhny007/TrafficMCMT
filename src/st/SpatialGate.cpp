#include "trafficmcmt/st/SpatialGate.h"

#include <cmath>

namespace tmcmt::st {
namespace {

constexpr double kEpsilon = 1e-9;

GateDecision Allow() noexcept {
    return GateDecision{true, GateReason::Allowed};
}

GateDecision Deny(GateReason reason) noexcept {
    return GateDecision{false, reason};
}

double Distance(const WorldPoint& lhs, const WorldPoint& rhs) noexcept {
    const double dx = static_cast<double>(lhs.x) - static_cast<double>(rhs.x);
    const double dy = static_cast<double>(lhs.y) - static_cast<double>(rhs.y);
    return std::hypot(dx, dy);
}

} // namespace

const char* ToString(GateReason reason) noexcept {
    switch (reason) {
    case GateReason::Allowed:
        return "allowed";
    case GateReason::SameCamera:
        return "same_camera";
    case GateReason::Unlinked:
        return "unlinked";
    case GateReason::OutsideWindow:
        return "outside_window";
    case GateReason::OutsideSigma:
        return "outside_sigma";
    case GateReason::MissingWorld:
        return "missing_world";
    case GateReason::MissingZone:
        return "missing_zone";
    case GateReason::ExitZone:
        return "exit_zone";
    case GateReason::EntryZone:
        return "entry_zone";
    case GateReason::Speed:
        return "speed";
    case GateReason::Distance:
        return "distance";
    }
    return "unknown";
}

SpatialGate::SpatialGate(const CameraLinkModel* model, SpatialGateOptions options)
    : m_model(model), m_options(options) {}

std::optional<TransitionStats> SpatialGate::ResolveTransition(const CameraId& from,
                                                              const CameraId& to) const {
    if (m_model == nullptr) {
        return std::nullopt;
    }
    return m_model->Transition(from, to);
}

GateDecision SpatialGate::Check(const TrackletEndpoint& from, const TrackletEndpoint& to) const {
    if (m_options.reject_same_camera && from.camera == to.camera) {
        return Deny(GateReason::SameCamera);
    }

    if (m_model != nullptr && !m_model->IsLinked(from.camera, to.camera)) {
        return Deny(GateReason::Unlinked);
    }

    const std::int64_t deltaMs = (to.begin - from.end).count();
    if (deltaMs < m_options.window.min_ms || deltaMs > m_options.window.max_ms) {
        return Deny(GateReason::OutsideWindow);
    }

    const std::optional<TransitionStats> transition = ResolveTransition(from.camera, to.camera);
    if (transition) {
        const double lower = transition->mean_ms - 3.0 * transition->stddev_ms;
        const double upper = transition->mean_ms + 3.0 * transition->stddev_ms;
        if (static_cast<double>(deltaMs) < lower || static_cast<double>(deltaMs) > upper) {
            return Deny(GateReason::OutsideSigma);
        }
    }

    switch (m_options.mode) {
    case GateMode::Off:
        return Allow();

    case GateMode::Zone: {
        if (!from.HasExit() || !to.HasEntry()) {
            return m_options.allow_missing_world ? Allow() : Deny(GateReason::MissingWorld);
        }
        if (m_model == nullptr) {
            return Deny(GateReason::MissingZone);
        }
        const CameraZones* fromZones = m_model->Zones(from.camera);
        const CameraZones* toZones = m_model->Zones(to.camera);
        if (fromZones == nullptr || !fromZones->exit_zone.IsValid() || toZones == nullptr ||
            !toZones->entry_zone.IsValid()) {
            return Deny(GateReason::MissingZone);
        }
        if (!fromZones->exit_zone.Contains(static_cast<double>(from.exit.x),
                                           static_cast<double>(from.exit.y))) {
            return Deny(GateReason::ExitZone);
        }
        if (!toZones->entry_zone.Contains(static_cast<double>(to.entry.x),
                                          static_cast<double>(to.entry.y))) {
            return Deny(GateReason::EntryZone);
        }
        return Allow();
    }

    case GateMode::Bev: {
        if (!from.HasExit() || !to.HasEntry()) {
            return m_options.allow_missing_world ? Allow() : Deny(GateReason::MissingWorld);
        }
        const double distance = Distance(from.exit, to.entry);
        if (distance > m_options.max_distance_m) {
            return Deny(GateReason::Distance);
        }
        const double deltaSeconds = static_cast<double>(deltaMs) / 1000.0;
        if (deltaSeconds > 0.0) {
            if (distance > m_options.max_speed_mps * deltaSeconds + kEpsilon) {
                return Deny(GateReason::Speed);
            }
        } else if (distance > kEpsilon) {
            return Deny(GateReason::Speed);
        }
        return Allow();
    }
    }

    return Allow();
}

} // namespace tmcmt::st
