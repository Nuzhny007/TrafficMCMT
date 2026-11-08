#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "trafficmcmt/reid/EmbeddingIndex.h"

namespace tmcmt::reid {

/// Exact inner-product index backed by a row-major `N x dim` matrix.
///
/// Storage is a flat `std::vector<float>` so that `Add` grows amortised;
/// the scan itself is an Eigen matrix-vector product, optionally split across
/// worker threads. Top-K is selected with `partial_sort`, which is O(N log k).
class BruteForceIndex final : public IEmbeddingIndex {
public:
    explicit BruteForceIndex(EmbeddingIndexOptions options);
    BruteForceIndex(int dim, std::size_t threads = 0);

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
        return m_dim;
    }
    [[nodiscard]] IndexKind Kind() const noexcept override {
        return IndexKind::Brute;
    }

private:
    void Clear();
    [[nodiscard]] std::size_t WorkerCount() const noexcept;
    [[nodiscard]] Result<std::vector<SearchHit>> SearchImpl(std::span<const float> query,
                                                            std::size_t k,
                                                            const CandidateFilter* filter) const;

    int m_dim = 0;
    std::size_t m_threads = 0;
    std::vector<float> m_values; // row-major, Size() * dim
    std::vector<TrackletId> m_ids;
    std::unordered_map<TrackletId, std::size_t> m_positions;
};

} // namespace tmcmt::reid
