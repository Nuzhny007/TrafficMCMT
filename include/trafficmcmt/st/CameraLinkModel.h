#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Types.h"
#include "trafficmcmt/st/Polygon.h"

namespace tmcmt::st {

/// Transition window used when no measured statistics are available.
struct TransitionWindow {
    std::int64_t min_ms = 0;
    std::int64_t max_ms = 120000;

    [[nodiscard]] bool IsValid() const noexcept {
        return min_ms >= 0 && max_ms > min_ms;
    }
};

/// Robust estimate of the end-to-end transition time between two cameras.
struct TransitionStats {
    double mean_ms = 0.0;
    double stddev_ms = 0.0;
    std::int64_t samples = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return std::isfinite(mean_ms) && std::isfinite(stddev_ms) && stddev_ms > 0.0 && samples >= 0;
    }
};

/// One directed edge of the camera topology.
struct CameraLink {
    CameraId from;
    CameraId to;
    /// Empty when the edge was declared without measured statistics; the
    /// fallback window is used instead.
    std::optional<TransitionStats> transition;
    bool enabled = true;
};

/// Entry/exit polygons of one camera, in shared ENU metres.
struct CameraZones {
    Polygon entry_zone;
    Polygon exit_zone;
};

/// Camera topology, transition statistics and entry/exit zones.
///
/// The model is loaded from a YAML file (see `configs/camera_links.yaml`).
/// `default_linked` is `false` by default: a pair of cameras is connected only
/// when it is explicitly listed in `links`. This keeps the gate conservative
/// and makes the topology auditable.
class CameraLinkModel {
public:
    [[nodiscard]] static Result<CameraLinkModel> Load(const std::string& path);
    [[nodiscard]] static Result<CameraLinkModel> LoadFromString(const std::string& text,
                                                                const std::string& source = "<memory>");

    [[nodiscard]] Status Save(const std::string& path) const;
    [[nodiscard]] std::string Dump() const;

    [[nodiscard]] bool DefaultLinked() const noexcept {
        return m_defaultLinked;
    }
    [[nodiscard]] const TransitionWindow& Fallback() const noexcept {
        return m_fallback;
    }

    /// Directed topology lookup. Same-camera and empty ids are never linked.
    [[nodiscard]] bool IsLinked(const CameraId& from, const CameraId& to) const;

    /// Measured transition statistics of an enabled edge, if any.
    [[nodiscard]] std::optional<TransitionStats> Transition(const CameraId& from, const CameraId& to) const;

    /// Zones of a camera, or nullptr when the camera has no zone section.
    [[nodiscard]] const CameraZones* Zones(const CameraId& camera) const;

    void SetDefaultLinked(bool value) noexcept {
        m_defaultLinked = value;
    }
    void SetFallback(const TransitionWindow& window) noexcept {
        m_fallback = window;
    }

    [[nodiscard]] Status SetLink(const CameraLink& link);
    [[nodiscard]] Status SetZones(const CameraId& camera, const CameraZones& zones);

    /// All explicit edges, sorted by `(from, to)`.
    [[nodiscard]] std::vector<CameraLink> Links() const;
    /// Cameras mentioned by `links` or `cameras`, sorted.
    [[nodiscard]] std::vector<CameraId> Cameras() const;
    [[nodiscard]] bool Empty() const noexcept {
        return m_links.empty() && m_zones.empty();
    }

private:
    using LinkKey = std::pair<CameraId, CameraId>;

    bool m_defaultLinked = false;
    TransitionWindow m_fallback{};
    std::map<LinkKey, CameraLink> m_links;
    std::map<CameraId, CameraZones> m_zones;
};

} // namespace tmcmt::st
