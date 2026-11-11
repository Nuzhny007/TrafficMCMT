#pragma once

#include <memory>
#include <string>

#include "trafficmcmt/core/Clock.h"
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

    /// Reads the next tracklet and reports the Master wall-clock time at which
    /// the message was received.
    ///
    /// The default implementation calls `Next(out)` and stamps `NowMs()`. Live
    /// sources override it so that the clock-synchronisation estimator sees the
    /// actual arrival time instead of the time at which the pipeline got around
    /// to consuming the item.
    [[nodiscard]] virtual Result<bool> Next(Tracklet& out, Timestamp& received_at) {
        auto result = Next(out);
        received_at = NowMs();
        return result;
    }

    /// Rewinds the stream when supported.
    [[nodiscard]] virtual Status Reset() = 0;

    /// Unblocks a pending `Next()` running on another thread. Default: no-op.
    virtual void Stop() noexcept {}

    [[nodiscard]] virtual std::string Describe() const = 0;
};

} // namespace tmcmt
