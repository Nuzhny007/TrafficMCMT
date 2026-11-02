#pragma once

#include <string>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt {

/// Consumer of tracklets (or of global track updates in later stages).
class ITrackletSink {
public:
    virtual ~ITrackletSink() = default;

    [[nodiscard]] virtual Status Write(const Tracklet& tracklet) = 0;
    [[nodiscard]] virtual Status Flush() = 0;
    [[nodiscard]] virtual std::string Describe() const = 0;
};

} // namespace tmcmt
