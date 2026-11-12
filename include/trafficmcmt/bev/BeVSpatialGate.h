#pragma once

// The gate itself lives in `trafficmcmt_st`, because it is a policy on top of
// `st::SpatialGate`. This forwarding header keeps the BEV-facing include path
// stable for callers that treat the gate as part of the world-coordinate
// module.
#include "trafficmcmt/st/BeVSpatialGate.h"

namespace tmcmt::bev {
using st::BeVSpatialGate;
} // namespace tmcmt::bev
