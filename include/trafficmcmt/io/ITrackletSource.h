#pragma once

#include <memory>
#include <string>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt {

/// Stream of tracklets. Implementations may read a file, a ZeroMQ socket or
/// generate synthetic data.
class ITrackletSource {
public:
    virtual ~ITrackletSource() = default;

    /// Reads the next tracklet.
    /// Returns `true` and fills `out` on success, `false` at the end of the
    /// stream, or an error status on malformed input.
    [[nodiscard]] virtual Result<bool> Next(Tracklet& out) = 0;

    /// Rewinds the stream when supported.
    [[nodiscard]] virtual Status Reset() = 0;

    [[nodiscard]] virtual std::string Describe() const = 0;
};

} // namespace tmcmt
