#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt {

/// Filter for `ITrackletStore::Query`.
struct TrackletQuery {
    /// Restrict to a single camera when set.
    std::optional<CameraId> camera;
    /// Inclusive lower bound on `Tracklet::begin`.
    std::optional<Timestamp> from;
    /// Inclusive upper bound on `Tracklet::begin`.
    std::optional<Timestamp> to;
    /// 0 means "no limit".
    std::size_t limit = 0;
};

/// History of ingested tracklets. The in-memory implementation is used by
/// tests and by the offline pipeline; a SQLite-backed implementation is added
/// in a later stage.
class ITrackletStore {
public:
    virtual ~ITrackletStore() = default;

    /// Inserts or replaces a tracklet with the same `id`.
    [[nodiscard]] virtual Status Add(const Tracklet& tracklet) = 0;

    [[nodiscard]] virtual std::optional<Tracklet> Get(TrackletId id) const = 0;

    [[nodiscard]] virtual std::vector<Tracklet> Query(const TrackletQuery& query) const = 0;

    [[nodiscard]] virtual std::size_t Size() const = 0;

    virtual void Clear() = 0;
};

} // namespace tmcmt
