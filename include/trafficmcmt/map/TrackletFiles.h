#pragma once

#include <string>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt::map {

/// Loads one or more JSONL/protobuf tracklet files without replay pacing.
[[nodiscard]] Result<std::vector<Tracklet>> LoadTrackletFiles(const std::vector<std::string>& paths);

} // namespace tmcmt::map
