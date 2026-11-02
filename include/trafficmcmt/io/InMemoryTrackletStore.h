#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/io/ITrackletStore.h"

namespace tmcmt {

/// RAM-only store with indexes by id, begin time and camera.
///
/// All operations are O(log n) or better. Tracklets are stored by value, so
/// the store is independent of the caller's lifetime.
class InMemoryTrackletStore final : public ITrackletStore {
public:
    [[nodiscard]] Status Add(const Tracklet& tracklet) override;
    [[nodiscard]] std::optional<Tracklet> Get(TrackletId id) const override;
    [[nodiscard]] std::vector<Tracklet> Query(const TrackletQuery& query) const override;
    [[nodiscard]] std::size_t Size() const override;
    void Clear() override;

private:
    using TimeIndex = std::map<Timestamp, std::vector<TrackletId>>;

    void RemoveFromIndexes(TrackletId id);

    std::unordered_map<TrackletId, Tracklet> m_byId;
    TimeIndex m_byTime;
    std::unordered_map<CameraId, TimeIndex> m_byCamera;
};

} // namespace tmcmt
