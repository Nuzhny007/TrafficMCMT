#include "trafficmcmt/map/TrackletFiles.h"

#include <utility>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/io/FileTrackletSource.h"

namespace tmcmt::map {

Result<std::vector<Tracklet>> LoadTrackletFiles(const std::vector<std::string>& paths) {
    std::vector<Tracklet> tracklets;
    for (const std::string& path : paths) {
        auto source = FileTrackletSource::Open(path, FileTrackletSource::Options{TrackletFileFormat::Auto, 0.0});
        if (!source) {
            return source.status();
        }
        while (true) {
            Tracklet tracklet;
            auto next = source.value()->Next(tracklet);
            if (!next) {
                return next.status();
            }
            if (!next.value()) {
                break;
            }
            tracklets.push_back(std::move(tracklet));
        }
    }
    return tracklets;
}

} // namespace tmcmt::map
