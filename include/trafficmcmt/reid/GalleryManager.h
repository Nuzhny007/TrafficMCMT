#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/reid/EmbeddingIndex.h"

namespace tmcmt::reid {

/// Metadata kept next to the indexed embedding.
struct GalleryEntry {
    TrackletId id = 0;
    CameraId camera;
    ObjectType type = ObjectType::Unknown;
    std::uint32_t model_id = 0;
    Timestamp begin{};
    Timestamp end{};
    float quality = 1.f;
    std::vector<float> values;
};

struct GalleryOptions {
    /// Drop entries whose `end` is older than `now - retention_ms`.
    /// 0 disables automatic eviction.
    std::int64_t retention_ms = 0;
    EmbeddingIndexOptions index;
};

/// Filters applied by `GalleryManager::Search`.
struct GalleryQuery {
    std::size_t top_k = 20;
    /// Camera of the query tracklet. Empty disables camera-based filtering.
    CameraId query_camera;
    bool exclude_same_camera = true;
    /// Restrict candidates to one camera.
    std::optional<CameraId> camera;
    /// Exclude the query tracklet itself.
    std::optional<TrackletId> exclude_id;
    /// Keep entries whose [begin, end] overlaps [from, to].
    std::optional<Timestamp> from;
    std::optional<Timestamp> to;
    std::optional<ObjectType> type;
    /// Restrict to embeddings produced by the same model.
    std::optional<std::uint32_t> model_id;
    float min_similarity = 0.f;
};

/// In-memory gallery of tracklet embeddings with per-camera buckets,
/// retention-based eviction and metadata filters.
///
/// The authoritative vectors live here; the index is a derived structure and
/// can be rebuilt from the gallery. This keeps removal and eviction exact
/// even for approximate backends.
class GalleryManager {
public:
    [[nodiscard]] static Result<std::unique_ptr<GalleryManager>> Create(IndexKind kind,
                                                                       GalleryOptions options);

    [[nodiscard]] Status Add(const Tracklet& tracklet);
    [[nodiscard]] Status Add(const GalleryEntry& entry);
    [[nodiscard]] Status Remove(TrackletId id);

    /// Removes entries older than `now - retention_ms`; returns the count.
    std::size_t Evict(Timestamp now);

    [[nodiscard]] Result<std::vector<SearchHit>> Search(std::span<const float> query,
                                                        const GalleryQuery& options) const;

    [[nodiscard]] const GalleryEntry* Get(TrackletId id) const noexcept;
    [[nodiscard]] std::vector<CameraId> Cameras() const;
    [[nodiscard]] std::size_t Size() const noexcept {
        return m_entries.size();
    }
    [[nodiscard]] int Dim() const noexcept {
        return m_index->Dim();
    }
    [[nodiscard]] IndexKind Kind() const noexcept {
        return m_index->Kind();
    }
    [[nodiscard]] const IEmbeddingIndex& Index() const noexcept {
        return *m_index;
    }

    void Clear();

private:
    GalleryManager(std::unique_ptr<IEmbeddingIndex> index, GalleryOptions options);

    std::unique_ptr<IEmbeddingIndex> m_index;
    std::int64_t m_retentionMs = 0;
    std::unordered_map<TrackletId, GalleryEntry> m_entries;
    std::unordered_map<CameraId, std::unordered_set<TrackletId>> m_byCamera;
    /// `end -> id`, ordered for cheap retention sweeps. Entries removed via
    /// `Remove` are left behind and skipped lazily by `Evict`.
    std::multimap<Timestamp, TrackletId> m_byEnd;
};

} // namespace tmcmt::reid
