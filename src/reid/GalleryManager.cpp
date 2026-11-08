#include "trafficmcmt/reid/GalleryManager.h"

#include <algorithm>
#include <utility>

namespace tmcmt::reid {

GalleryManager::GalleryManager(std::unique_ptr<IEmbeddingIndex> index, GalleryOptions options)
    : m_index(std::move(index)), m_retentionMs(options.retention_ms) {}

Result<std::unique_ptr<GalleryManager>> GalleryManager::Create(IndexKind kind,
                                                              GalleryOptions options) {
    auto index = CreateEmbeddingIndex(kind, options.index);
    if (!index) {
        return index.status();
    }
    return std::unique_ptr<GalleryManager>(new GalleryManager(std::move(index).value(), options));
}

Status GalleryManager::Add(const Tracklet& tracklet) {
    if (!tracklet.embedding.IsValid()) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "tracklet " + std::to_string(tracklet.id) + " has no valid embedding");
    }
    GalleryEntry entry;
    entry.id = tracklet.id;
    entry.camera = tracklet.camera;
    entry.type = tracklet.type;
    entry.model_id = tracklet.embedding.model_id;
    entry.begin = tracklet.begin;
    entry.end = tracklet.end;
    entry.quality = tracklet.quality;
    entry.values = tracklet.embedding.values;
    return Add(entry);
}

Status GalleryManager::Add(const GalleryEntry& entry) {
    if (entry.camera.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "gallery entry has an empty camera id");
    }
    if (entry.values.size() != static_cast<std::size_t>(m_index->Dim())) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "gallery entry " + std::to_string(entry.id) + " has dimension " +
                                 std::to_string(entry.values.size()) + ", expected " +
                                 std::to_string(m_index->Dim()));
    }

    // Index first: if it rejects the vector (non-finite / wrong dimension) the
    // metadata is not modified.
    Status status = m_index->Add(entry.id, entry.values);
    if (!status) {
        return status;
    }

    const auto existing = m_entries.find(entry.id);
    if (existing != m_entries.end()) {
        const auto oldBucket = m_byCamera.find(existing->second.camera);
        if (oldBucket != m_byCamera.end()) {
            oldBucket->second.erase(entry.id);
            if (oldBucket->second.empty()) {
                m_byCamera.erase(oldBucket);
            }
        }
    }
    m_byEnd.emplace(entry.end, entry.id);
    m_byCamera[entry.camera].insert(entry.id);
    m_entries.insert_or_assign(entry.id, entry);
    return Status::Ok();
}

Status GalleryManager::Remove(TrackletId id) {
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return Status::Error(ErrorCode::NotFound, "tracklet " + std::to_string(id) + " is not in the gallery");
    }

    Status status = m_index->Remove(id);
    if (!status) {
        return status;
    }

    auto bucket = m_byCamera.find(it->second.camera);
    if (bucket != m_byCamera.end()) {
        bucket->second.erase(id);
        if (bucket->second.empty()) {
            m_byCamera.erase(bucket);
        }
    }
    m_entries.erase(it);
    return Status::Ok();
}

std::size_t GalleryManager::Evict(Timestamp now) {
    if (m_retentionMs <= 0 || m_byEnd.empty()) {
        return 0;
    }

    const Timestamp cutoff = now - std::chrono::milliseconds(m_retentionMs);
    std::size_t removed = 0;
    for (auto it = m_byEnd.begin(); it != m_byEnd.end() && it->first < cutoff;) {
        const TrackletId id = it->second;
        const auto entry = m_entries.find(id);
        // Skip entries that were removed or re-added with a newer end.
        if (entry != m_entries.end() && entry->second.end < cutoff) {
            (void)m_index->Remove(id);
            auto bucket = m_byCamera.find(entry->second.camera);
            if (bucket != m_byCamera.end()) {
                bucket->second.erase(id);
                if (bucket->second.empty()) {
                    m_byCamera.erase(bucket);
                }
            }
            m_entries.erase(entry);
            ++removed;
        }
        it = m_byEnd.erase(it);
    }
    return removed;
}

Result<std::vector<SearchHit>> GalleryManager::Search(std::span<const float> query,
                                                      const GalleryQuery& options) const {
    if (options.top_k == 0) {
        return std::vector<SearchHit>{};
    }

    const CandidateFilter filter = [this, &options](TrackletId id) {
        if (options.exclude_id && *options.exclude_id == id) {
            return false;
        }
        const auto entry = m_entries.find(id);
        if (entry == m_entries.end()) {
            return false;
        }

        if (options.camera) {
            const auto bucket = m_byCamera.find(*options.camera);
            if (bucket == m_byCamera.end() || bucket->second.find(id) == bucket->second.end()) {
                return false;
            }
        }
        if (options.exclude_same_camera && !options.query_camera.empty()) {
            const auto bucket = m_byCamera.find(options.query_camera);
            if (bucket != m_byCamera.end() && bucket->second.find(id) != bucket->second.end()) {
                return false;
            }
        }
        if (options.model_id && entry->second.model_id != *options.model_id) {
            return false;
        }
        if (options.type && entry->second.type != *options.type) {
            return false;
        }
        if (options.from && entry->second.end < *options.from) {
            return false;
        }
        if (options.to && entry->second.begin > *options.to) {
            return false;
        }
        return true;
    };

    auto hits = m_index->Search(query, options.top_k, filter);
    if (!hits) {
        return hits.status();
    }

    std::vector<SearchHit> filtered;
    filtered.reserve(hits.value().size());
    for (const SearchHit& hit : hits.value()) {
        if (hit.score >= options.min_similarity) {
            filtered.push_back(hit);
        }
    }
    return filtered;
}

const GalleryEntry* GalleryManager::Get(TrackletId id) const noexcept {
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : &it->second;
}

std::vector<CameraId> GalleryManager::Cameras() const {
    std::vector<CameraId> cameras;
    cameras.reserve(m_byCamera.size());
    for (const auto& [camera, ids] : m_byCamera) {
        cameras.push_back(camera);
    }
    std::sort(cameras.begin(), cameras.end());
    return cameras;
}

void GalleryManager::Clear() {
    m_entries.clear();
    m_byCamera.clear();
    m_byEnd.clear();
    (void)m_index->Rebuild(std::span<const IndexEntry>{});
}

} // namespace tmcmt::reid
