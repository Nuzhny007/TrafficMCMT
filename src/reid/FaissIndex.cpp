#include "trafficmcmt/reid/FaissIndex.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <unordered_set>
#include <utility>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/IndexIDMap.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/impl/FaissException.h>
#include <faiss/impl/IDSelector.h>

namespace tmcmt::reid {
namespace {

constexpr std::int64_t kMaxFaissId = std::numeric_limits<std::int64_t>::max();

bool AreFinite(std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

} // namespace

FaissIndex::FaissIndex(IndexKind kind, EmbeddingIndexOptions options)
    : m_kind(kind), m_options(std::move(options)) {}

FaissIndex::~FaissIndex() = default;

Result<std::unique_ptr<IEmbeddingIndex>> FaissIndex::Create(IndexKind kind,
                                                            EmbeddingIndexOptions options) {
    if (options.dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "FAISS index dimension must be > 0");
    }
    if (kind != IndexKind::FaissFlat && kind != IndexKind::FaissIvfPq && kind != IndexKind::FaissHnsw) {
        return Status::Error(ErrorCode::Unsupported, "unsupported FAISS index kind");
    }

    std::unique_ptr<FaissIndex> index(new FaissIndex(kind, std::move(options)));
    try {
        if (kind != IndexKind::FaissIvfPq) {
            Status status = index->ResetIndex(0);
            if (!status) {
                return status;
            }
        }
    } catch (const faiss::FaissException& error) {
        return Status::Error(ErrorCode::Internal, std::string("FAISS init failed: ") + error.what());
    }
    return std::unique_ptr<IEmbeddingIndex>(std::move(index));
}

int FaissIndex::PqSubQuantizers() const noexcept {
    const int dim = m_options.dim;
    if (m_options.pq_m > 0 && dim % m_options.pq_m == 0) {
        return m_options.pq_m;
    }
    int best = 1;
    for (int sub = 1; sub <= std::min(dim, 64); ++sub) {
        if (dim % sub != 0) {
            continue;
        }
        const int dsub = dim / sub;
        if (dsub >= 4 && dsub <= 16) {
            best = std::max(best, sub);
        }
    }
    if (best > 1) {
        return best;
    }
    for (int sub = std::min(dim, 64); sub >= 1; --sub) {
        if (dim % sub == 0) {
            return sub;
        }
    }
    return 1;
}

Status FaissIndex::ResetIndex(std::size_t training_vectors) {
    m_index.reset();
    m_trained = false;

    if (m_kind == IndexKind::FaissFlat) {
        auto* flat = new faiss::IndexFlatIP(m_options.dim);
        auto* map = new faiss::IndexIDMap2(flat);
        map->own_fields = true;
        m_index.reset(map);
        m_trained = true;
        return Status::Ok();
    }

    if (m_kind == IndexKind::FaissHnsw) {
        auto* hnsw = new faiss::IndexHNSWFlat(m_options.dim, m_options.hnsw_m,
                                              faiss::METRIC_INNER_PRODUCT);
        auto* map = new faiss::IndexIDMap2(hnsw);
        map->own_fields = true;
        m_index.reset(map);
        m_trained = true;
        return Status::Ok();
    }

    if (training_vectors == 0) {
        return Status::Ok();
    }

    const int nlist = std::max(1, std::min(m_options.nlist, static_cast<int>(training_vectors)));
    auto* quantizer = new faiss::IndexFlatIP(m_options.dim);
    auto* ivf = new faiss::IndexIVFPQ(quantizer, m_options.dim, nlist, PqSubQuantizers(),
                                      m_options.pq_nbits, faiss::METRIC_INNER_PRODUCT);
    ivf->own_fields = true;
    ivf->nprobe = std::max(1, std::min(m_options.nprobe, nlist));
    ivf->train(static_cast<faiss::idx_t>(training_vectors), m_pendingValues.data());

    auto* map = new faiss::IndexIDMap2(ivf);
    map->own_fields = true;
    m_index.reset(map);
    m_trained = true;
    return Status::Ok();
}

Status FaissIndex::AddToIndex(std::size_t count, const float* values, const std::int64_t* ids) {
    if (m_index == nullptr || !m_trained) {
        return Status::Error(ErrorCode::Internal, "FAISS index is not trained");
    }
    std::vector<faiss::idx_t> labels(count);
    for (std::size_t i = 0; i < count; ++i) {
        labels[i] = static_cast<faiss::idx_t>(ids[i]);
    }
    m_index->add_with_ids(static_cast<faiss::idx_t>(count), values, labels.data());
    return Status::Ok();
}

Status FaissIndex::Add(TrackletId id, std::span<const float> values) {
    if (m_options.dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "FAISS index dimension must be > 0");
    }
    if (values.size() != static_cast<std::size_t>(m_options.dim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "embedding dimension mismatch: index expects " +
                                 std::to_string(m_options.dim) + ", got " +
                                 std::to_string(values.size()));
    }
    if (!AreFinite(values)) {
        return Status::Error(ErrorCode::InvalidArgument, "embedding contains a non-finite value");
    }
    if (id > static_cast<TrackletId>(kMaxFaissId)) {
        return Status::Error(ErrorCode::InvalidArgument, "tracklet id does not fit into a FAISS id");
    }

    try {
        if (m_ids.find(static_cast<std::int64_t>(id)) != m_ids.end()) {
            Status status = Remove(id);
            if (!status) {
                return status;
            }
        }

        if (m_kind == IndexKind::FaissIvfPq && !m_trained) {
            m_pendingIds.push_back(static_cast<std::int64_t>(id));
            m_pendingValues.insert(m_pendingValues.end(), values.begin(), values.end());
            m_ids.insert(static_cast<std::int64_t>(id));
            return Status::Ok();
        }

        const std::int64_t label = static_cast<std::int64_t>(id);
        Status status = AddToIndex(1, values.data(), &label);
        if (!status) {
            return status;
        }
        m_ids.insert(label);
        return Status::Ok();
    } catch (const faiss::FaissException& error) {
        return Status::Error(ErrorCode::Internal, std::string("FAISS add failed: ") + error.what());
    }
}

Status FaissIndex::Remove(TrackletId id) {
    if (id > static_cast<TrackletId>(kMaxFaissId)) {
        return Status::Error(ErrorCode::NotFound, "tracklet id is not a FAISS id");
    }
    const auto label = static_cast<std::int64_t>(id);
    if (m_ids.find(label) == m_ids.end()) {
        return Status::Error(ErrorCode::NotFound, "tracklet " + std::to_string(id) + " is not indexed");
    }

    try {
        if (m_kind == IndexKind::FaissIvfPq && !m_trained) {
            const auto position =
                std::find(m_pendingIds.begin(), m_pendingIds.end(), label);
            if (position != m_pendingIds.end()) {
                const std::size_t offset =
                    static_cast<std::size_t>(std::distance(m_pendingIds.begin(), position));
                const auto dim = static_cast<std::ptrdiff_t>(m_options.dim);
                m_pendingValues.erase(
                    m_pendingValues.begin() + static_cast<std::ptrdiff_t>(offset) * dim,
                    m_pendingValues.begin() + static_cast<std::ptrdiff_t>(offset + 1) * dim);
                m_pendingIds.erase(position);
            }
        } else if (m_index != nullptr) {
            const faiss::idx_t selector_id = static_cast<faiss::idx_t>(label);
            faiss::IDSelectorBatch selector(1, &selector_id);
            m_index->remove_ids(selector);
        }
    } catch (const faiss::FaissException& error) {
        return Status::Error(ErrorCode::Internal, std::string("FAISS remove failed: ") + error.what());
    }

    m_ids.erase(label);
    return Status::Ok();
}

Status FaissIndex::Rebuild(std::span<const IndexEntry> entries) {
    if (m_options.dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "FAISS index dimension must be > 0");
    }

    std::vector<float> values;
    std::vector<std::int64_t> ids;
    std::unordered_set<std::int64_t> seen;
    values.reserve(entries.size() * static_cast<std::size_t>(m_options.dim));
    ids.reserve(entries.size());
    seen.reserve(entries.size());
    for (const IndexEntry& entry : entries) {
        if (entry.values.size() != static_cast<std::size_t>(m_options.dim)) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "rebuild entry " + std::to_string(entry.id) + " has dimension " +
                                     std::to_string(entry.values.size()) + ", expected " +
                                     std::to_string(m_options.dim));
        }
        if (!AreFinite(entry.values)) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "rebuild entry " + std::to_string(entry.id) +
                                     " contains a non-finite value");
        }
        if (entry.id > static_cast<TrackletId>(kMaxFaissId)) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "tracklet id does not fit into a FAISS id");
        }
        if (!seen.insert(static_cast<std::int64_t>(entry.id)).second) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "duplicate tracklet id " + std::to_string(entry.id) +
                                     " in rebuild batch");
        }
        ids.push_back(static_cast<std::int64_t>(entry.id));
        values.insert(values.end(), entry.values.begin(), entry.values.end());
    }

    try {
        m_index.reset();
        m_trained = false;
        m_ids.clear();
        m_pendingValues = std::move(values);
        m_pendingIds = std::move(ids);
        for (const std::int64_t id : m_pendingIds) {
            m_ids.insert(id);
        }

        Status status = ResetIndex(m_pendingIds.size());
        if (!status) {
            return status;
        }
        if (!m_pendingIds.empty()) {
            status = AddToIndex(m_pendingIds.size(), m_pendingValues.data(), m_pendingIds.data());
            if (!status) {
                return status;
            }
        }
        m_pendingValues.clear();
        m_pendingIds.clear();
        return Status::Ok();
    } catch (const faiss::FaissException& error) {
        return Status::Error(ErrorCode::Internal, std::string("FAISS rebuild failed: ") + error.what());
    }
}

Result<std::vector<SearchHit>> FaissIndex::Search(std::span<const float> query, std::size_t k) const {
    return SearchImpl(query, k, nullptr);
}

Result<std::vector<SearchHit>> FaissIndex::Search(std::span<const float> query, std::size_t k,
                                                  const CandidateFilter& filter) const {
    return SearchImpl(query, k, &filter);
}

Result<std::vector<SearchHit>> FaissIndex::SearchImpl(std::span<const float> query, std::size_t k,
                                                      const CandidateFilter* filter) const {
    if (k == 0) {
        return std::vector<SearchHit>{};
    }
    if (query.size() != static_cast<std::size_t>(m_options.dim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "query dimension mismatch: index expects " +
                                 std::to_string(m_options.dim) + ", got " +
                                 std::to_string(query.size()));
    }
    if (!AreFinite(query)) {
        return Status::Error(ErrorCode::InvalidArgument, "query contains a non-finite value");
    }
    if (m_ids.empty()) {
        return std::vector<SearchHit>{};
    }

    if (m_kind == IndexKind::FaissIvfPq && !m_trained) {
        // Linear fallback over the vectors buffered before training.
        const std::size_t count = m_pendingIds.size();
        std::vector<float> scores(count, 0.f);
        for (std::size_t i = 0; i < count; ++i) {
            const float* row = m_pendingValues.data() + i * static_cast<std::size_t>(m_options.dim);
            float score = 0.f;
            for (int j = 0; j < m_options.dim; ++j) {
                score += row[j] * query[static_cast<std::size_t>(j)];
            }
            scores[i] = score;
        }

        std::vector<std::size_t> candidates;
        candidates.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (filter == nullptr || (*filter)(static_cast<TrackletId>(m_pendingIds[i]))) {
                candidates.push_back(i);
            }
        }
        if (candidates.empty()) {
            return std::vector<SearchHit>{};
        }
        const std::size_t take = std::min(k, candidates.size());
        std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(take),
                          candidates.end(), [&scores, this](std::size_t lhs, std::size_t rhs) {
                              if (scores[lhs] != scores[rhs]) {
                                  return scores[lhs] > scores[rhs];
                              }
                              return m_pendingIds[lhs] < m_pendingIds[rhs];
                          });
        std::vector<SearchHit> hits;
        hits.reserve(take);
        for (std::size_t i = 0; i < take; ++i) {
            const std::size_t position = candidates[i];
            hits.push_back(SearchHit{static_cast<TrackletId>(m_pendingIds[position]),
                                     scores[position]});
        }
        return hits;
    }

    if (m_index == nullptr) {
        return std::vector<SearchHit>{};
    }

    try {
        const std::size_t total = static_cast<std::size_t>(m_index->ntotal);
        if (total == 0) {
            return std::vector<SearchHit>{};
        }
        std::size_t want = std::min(k, total);
        std::vector<faiss::idx_t> labels;
        std::vector<float> distances;
        std::vector<SearchHit> hits;

        for (;;) {
            labels.assign(want, -1);
            distances.assign(want, 0.f);
            m_index->search(1, query.data(), static_cast<faiss::idx_t>(want), distances.data(),
                            labels.data());

            hits.clear();
            for (std::size_t i = 0; i < want; ++i) {
                if (labels[i] < 0) {
                    continue;
                }
                const auto id = static_cast<TrackletId>(labels[i]);
                if (filter != nullptr && !(*filter)(id)) {
                    continue;
                }
                hits.push_back(SearchHit{id, distances[i]});
                if (hits.size() >= k) {
                    break;
                }
            }
            if (hits.size() >= k || want >= total) {
                break;
            }
            want = std::min(total, want * 2);
        }
        return hits;
    } catch (const faiss::FaissException& error) {
        return Status::Error(ErrorCode::Internal, std::string("FAISS search failed: ") + error.what());
    }
}

} // namespace tmcmt::reid
