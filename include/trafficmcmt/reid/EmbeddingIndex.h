#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "trafficmcmt/core/Config.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Types.h"

namespace tmcmt::reid {

/// One neighbour returned by an embedding index.
struct SearchHit {
    TrackletId id = 0;
    /// Inner product. For L2-normalised embeddings this is the cosine
    /// similarity in [-1, 1].
    float score = 0.f;
};

/// A (id, vector) pair used by `IEmbeddingIndex::Rebuild`.
struct IndexEntry {
    TrackletId id = 0;
    std::span<const float> values;
};

/// Optional filter applied to candidate ids before scoring. Used by the
/// gallery to exclude the query camera, self-matches and foreign model ids.
using CandidateFilter = std::function<bool(TrackletId)>;

struct EmbeddingIndexOptions {
    int dim = 512;
    /// Worker threads for the brute-force scan. 0 selects a heuristic based
    /// on the index size; 1 disables threading.
    std::size_t threads = 0;
    /// FAISS IVF parameters.
    int nlist = 4096;
    int nprobe = 32;
    /// FAISS HNSW connectivity.
    int hnsw_m = 32;
    /// FAISS PQ sub-quantisers. 0 lets the backend pick a divisor of `dim`.
    int pq_m = 0;
    int pq_nbits = 8;
};

/// Vector index over appearance embeddings.
///
/// Implementations own their storage. `Add` replaces an existing id, `Remove`
/// reports `NotFound` for an unknown id, and `Rebuild` atomically replaces the
/// whole content (retraining the IVF quantiser for FAISS).
class IEmbeddingIndex {
public:
    virtual ~IEmbeddingIndex() = default;

    [[nodiscard]] virtual Status Add(TrackletId id, std::span<const float> values) = 0;
    [[nodiscard]] virtual Status Remove(TrackletId id) = 0;
    [[nodiscard]] virtual Status Rebuild(std::span<const IndexEntry> entries) = 0;

    /// Returns at most `k` hits ordered by descending score. Ties are broken
    /// by ascending id so that results are deterministic.
    [[nodiscard]] virtual Result<std::vector<SearchHit>> Search(std::span<const float> query,
                                                               std::size_t k) const = 0;

    /// Filtered variant. `filter` is evaluated before scoring; implementations
    /// that cannot push a predicate down (FAISS) oversample internally.
    [[nodiscard]] virtual Result<std::vector<SearchHit>> Search(
        std::span<const float> query, std::size_t k, const CandidateFilter& filter) const = 0;

    [[nodiscard]] virtual std::size_t Size() const noexcept = 0;
    [[nodiscard]] virtual int Dim() const noexcept = 0;
    [[nodiscard]] virtual IndexKind Kind() const noexcept = 0;
};

/// True when the binary was compiled with `TRAFFICMCMT_WITH_FAISS=ON`.
[[nodiscard]] bool IsFaissAvailable() noexcept;

/// Creates an index. Requesting a `faiss_*` kind in a build without FAISS
/// returns `Unsupported` instead of silently falling back to brute force.
[[nodiscard]] Result<std::unique_ptr<IEmbeddingIndex>> CreateEmbeddingIndex(
    IndexKind kind, EmbeddingIndexOptions options);

} // namespace tmcmt::reid
