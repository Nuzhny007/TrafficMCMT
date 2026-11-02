#include "trafficmcmt/io/InMemoryTrackletStore.h"

#include <algorithm>
#include <utility>

#include "trafficmcmt/core/TrackletId.h"

namespace tmcmt {
namespace {

template <typename Index>
void EraseFromIndex(Index& index, Timestamp ts, TrackletId id) {
    auto it = index.find(ts);
    if (it == index.end()) {
        return;
    }
    auto& ids = it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (ids.empty()) {
        index.erase(it);
    }
}

template <typename Index>
void Collect(const Index& index, const TrackletQuery& query,
             const std::unordered_map<TrackletId, Tracklet>& byId, std::vector<Tracklet>& out) {
    auto it = index.begin();
    if (query.from) {
        it = index.lower_bound(*query.from);
    }
    for (; it != index.end(); ++it) {
        if (query.to && it->first > *query.to) {
            break;
        }
        for (const TrackletId id : it->second) {
            const auto found = byId.find(id);
            if (found != byId.end()) {
                out.push_back(found->second);
                if (query.limit != 0 && out.size() >= query.limit) {
                    return;
                }
            }
        }
    }
}

} // namespace

Status InMemoryTrackletStore::Add(const Tracklet& tracklet) {
    Tracklet stored = tracklet;
    if (stored.id == 0) {
        AssignTrackletId(stored);
    }
    if (stored.id == 0) {
        return Status::Error(ErrorCode::InvalidArgument, "tracklet id must not be zero");
    }

    if (m_byId.find(stored.id) != m_byId.end()) {
        RemoveFromIndexes(stored.id);
    }

    const TrackletId id = stored.id;
    const Timestamp begin = stored.begin;
    const CameraId camera = stored.camera;

    m_byId.emplace(id, std::move(stored));
    m_byTime[begin].push_back(id);
    m_byCamera[camera][begin].push_back(id);
    return Status::Ok();
}

std::optional<Tracklet> InMemoryTrackletStore::Get(TrackletId id) const {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<Tracklet> InMemoryTrackletStore::Query(const TrackletQuery& query) const {
    std::vector<Tracklet> out;

    if (query.camera) {
        const auto it = m_byCamera.find(*query.camera);
        if (it == m_byCamera.end()) {
            return out;
        }
        Collect(it->second, query, m_byId, out);
        return out;
    }

    Collect(m_byTime, query, m_byId, out);
    return out;
}

std::size_t InMemoryTrackletStore::Size() const {
    return m_byId.size();
}

void InMemoryTrackletStore::Clear() {
    m_byId.clear();
    m_byTime.clear();
    m_byCamera.clear();
}

void InMemoryTrackletStore::RemoveFromIndexes(TrackletId id) {
    const auto it = m_byId.find(id);
    if (it == m_byId.end()) {
        return;
    }
    const Tracklet& tracklet = it->second;
    EraseFromIndex(m_byTime, tracklet.begin, id);

    const auto cameraIt = m_byCamera.find(tracklet.camera);
    if (cameraIt != m_byCamera.end()) {
        EraseFromIndex(cameraIt->second, tracklet.begin, id);
        if (cameraIt->second.empty()) {
            m_byCamera.erase(cameraIt);
        }
    }

    m_byId.erase(it);
}

} // namespace tmcmt
