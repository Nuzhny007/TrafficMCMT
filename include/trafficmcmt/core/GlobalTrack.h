#pragma once

#include <string>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// Snapshot of one global identity. Each segment is a single-camera tracklet
/// chunk that belongs to `global_id`; the set may grow while the object is
/// visible and becomes final when `is_final` is set.
struct GlobalTrackUpdate {
    GlobalId global_id = kInvalidGlobalId;
    std::vector<Tracklet> segments;
    ObjectType type = ObjectType::Unknown;
    bool is_final = false;
    Timestamp updated_at{};
};

/// Emitted whenever two already assigned identities are merged.
struct GlobalIdMergeEvent {
    GlobalId old_id = kInvalidGlobalId;
    GlobalId new_id = kInvalidGlobalId;
    std::string reason;
    Timestamp ts{};
};

} // namespace tmcmt
