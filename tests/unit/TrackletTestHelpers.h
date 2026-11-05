#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/TrackletId.h"

namespace tmcmt::test {

/// Builds a valid tracklet that satisfies all protocol invariants.
inline Tracklet MakeTracklet(std::string session_id = "session-a", std::string camera = "cam-01",
                             std::uint64_t local_track_id = 7, std::uint64_t sequence = 0,
                             std::int64_t begin_ms = 1'700'000'000'000LL) {
    Tracklet tracklet;
    tracklet.session_id = std::move(session_id);
    tracklet.camera = std::move(camera);
    tracklet.local_track_id = local_track_id;
    tracklet.sequence = sequence;
    tracklet.begin = Timestamp(begin_ms);
    tracklet.end = Timestamp(begin_ms + 100);

    Observation first;
    first.ts = tracklet.begin;
    first.bbox = BBox{10.f, 20.f, 30.f, 40.f, 0.9f};
    first.world = WorldPoint{1.5f, -2.5f, true};

    Observation second;
    second.ts = tracklet.end;
    second.bbox = BBox{12.f, 22.f, 30.f, 40.f, 0.8f};

    tracklet.observations = {first, second};
    tracklet.embedding.values = {0.5f, 0.5f, 0.5f, 0.5f};
    tracklet.embedding.dim = 4;
    tracklet.embedding.model_id = 7;
    tracklet.type = ObjectType::Car;
    tracklet.quality = 0.75f;
    tracklet.schema_version = 1;
    tracklet.origin_latitude = 32.606456;
    tracklet.origin_longitude = -85.481891;

    AssignTrackletId(tracklet);
    return tracklet;
}

} // namespace tmcmt::test
