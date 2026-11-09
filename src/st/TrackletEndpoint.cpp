#include "trafficmcmt/st/TrackletEndpoint.h"

#include "trafficmcmt/reid/GalleryManager.h"

namespace tmcmt::st {

TrackletEndpoint MakeEndpoint(const Tracklet& tracklet) {
    TrackletEndpoint endpoint;
    endpoint.id = tracklet.id;
    endpoint.camera = tracklet.camera;
    endpoint.type = tracklet.type;
    endpoint.begin = tracklet.begin;
    endpoint.end = tracklet.end;

    for (const Observation& observation : tracklet.observations) {
        if (!observation.world.IsFinite()) {
            continue;
        }
        if (!endpoint.entry.IsFinite()) {
            endpoint.entry = observation.world;
        }
        endpoint.exit = observation.world;
    }
    return endpoint;
}

TrackletEndpoint MakeEndpoint(const reid::GalleryEntry& entry) {
    TrackletEndpoint endpoint;
    endpoint.id = entry.id;
    endpoint.camera = entry.camera;
    endpoint.type = entry.type;
    endpoint.begin = entry.begin;
    endpoint.end = entry.end;
    endpoint.entry = entry.entry_world;
    endpoint.exit = entry.exit_world;
    return endpoint;
}

} // namespace tmcmt::st
