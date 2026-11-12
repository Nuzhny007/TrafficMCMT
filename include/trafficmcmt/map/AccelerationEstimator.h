#pragma once

// Compatibility header: the implementation moved to trafficmcmt_geo so that
// the BEV module can share it without linking the map/protobuf stack.
#include "trafficmcmt/geo/AccelerationEstimator.h"

namespace tmcmt::map {
using geo::AccelerationEstimator;
} // namespace tmcmt::map
