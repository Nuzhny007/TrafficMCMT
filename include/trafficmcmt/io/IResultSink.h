#pragma once

#include <string>

#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"

namespace tmcmt {

/// Consumer of global track updates and identity merge events.
class IResultSink {
public:
    virtual ~IResultSink() = default;

    [[nodiscard]] virtual Status Publish(const GlobalTrackUpdate& update) = 0;
    [[nodiscard]] virtual Status PublishMerge(const GlobalIdMergeEvent& event) = 0;
    [[nodiscard]] virtual Status Flush() = 0;
    [[nodiscard]] virtual std::string Describe() const = 0;
};

} // namespace tmcmt
