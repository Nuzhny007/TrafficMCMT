#pragma once

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "trafficmcmt/reid/EmbeddingIndex.h"

namespace faiss {
class Index;
} // namespace faiss

namespace tmcmt::reid {

/// FAISS-backed index: `IndexFlatIP`, `IndexIVFPQ` and `IndexHNSWFlat`,
/// wrapped in `IndexIDMap2` so that tracklet ids survive search results.
///
/// Only compiled when `TRAFFICMCMT_WITH_FAISS=ON`. The brute-force index stays
/// the default and the reference implementation for tests.
class FaissIndex final : public IEmbeddingIndex {
public:
    [[nodiscard]] static Result<std::unique_ptr<IEmbeddingIndex>> Create(
        IndexKind kind, EmbeddingIndexOptions options);

    ~FaissIndex() override;

    [[nodiscard]] Status Add(TrackletId id, std::span<const float> values) override;
    [[nodiscard]] Status Remove(TrackletId id) override;
    [[nodiscard]] Status Rebuild(std::span<const IndexEntry> entries) override;

    [[nodiscard]] Result<std::vector<SearchHit>> Search(std::span<const float> query,
                                                        std::size_t k) const override;
    [[nodiscard]] Result<std::vector<SearchHit>> Search(std::span<const float> query, std::size_t k,
                                                        const CandidateFilter& filter) const override;

    [[nodiscard]] std::size_t Size() const noexcept override {
        return m_ids.size();
    }
    [[nodiscard]] int Dim() const noexcept override {
        return m_options.dim;
    }
    [[nodiscard]] IndexKind Kind() const noexcept override {
        return m_kind;
    }

private:
    FaissIndex(IndexKind kind, EmbeddingIndexOptions options);

    [[nodiscard]] Status ResetIndex(std::size_t training_vectors);
    [[nodiscard]] Status AddToIndex(std::size_t count, const float* values, const std::int64_t* ids);
    [[nodiscard]] Result<std::vector<SearchHit>> SearchImpl(std::span<const float> query, std::size_t k,
                                                            const CandidateFilter* filter) const;

    [[nodiscard]] int PqSubQuantizers() const noexcept;

    IndexKind m_kind;
    EmbeddingIndexOptions m_options;
    std::unique_ptr<faiss::Index> m_index;
    bool m_trained = false;

    std::unordered_set<std::int64_t> m_ids;

    /// IVF indices need training data. Vectors added before training are kept
    /// here and searched linearly until `Rebuild` (or enough data) trains the
    /// quantiser.
    std::vector<float> m_pendingValues;
    std::vector<std::int64_t> m_pendingIds;
};

} // namespace tmcmt::reid
