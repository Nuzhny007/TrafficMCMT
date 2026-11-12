#include "trafficmcmt/st/BeVSpatialGate.h"

#include "trafficmcmt/core/Config.h"

namespace tmcmt::st {

BeVSpatialGate::BeVSpatialGate(const CameraLinkModel* model, SpatialGateOptions options,
                               bool fallback_to_zone)
    : m_preferred(model, options),
      m_zone(model, [&options] {
          SpatialGateOptions zone = options;
          zone.mode = GateMode::Zone;
          return zone;
      }()),
      m_fallbackToZone(fallback_to_zone) {}

GateDecision BeVSpatialGate::Check(const TrackletEndpoint& from, const TrackletEndpoint& to) const {
    if (m_preferred.Options().mode != GateMode::Bev) {
        return m_preferred.Check(from, to);
    }
    if (from.HasWorld() && to.HasWorld()) {
        return m_preferred.Check(from, to);
    }
    if (m_fallbackToZone) {
        return m_zone.Check(from, to);
    }
    return m_preferred.Check(from, to);
}

} // namespace tmcmt::st
