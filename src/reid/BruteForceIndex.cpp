#include "trafficmcmt/reid/BruteForceIndex.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <thread>
#include <utility>

#include <Eigen/Core>

namespace tmcmt::reid {
namespace {

/// Below this size a single thread is faster than spawning workers.
constexpr std::size_t kParallelThreshold = 20000;
constexpr unsigned kMaxWorkers = 8;

bool AreFinite(std::span<const float> values) {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}

} // namespace

BruteForceIndex::BruteForceIndex(EmbeddingIndexOptions options)
    : m_dim(options.dim), m_threads(options.threads) {}

BruteForceIndex::BruteForceIndex(int dim, std::size_t threads) : m_dim(dim), m_threads(threads) {}

void BruteForceIndex::Clear() {
    m_values.clear();
    m_ids.clear();
    m_positions.clear();
}

std::size_t BruteForceIndex::WorkerCount() const noexcept {
    if (m_threads == 1) {
        return 1;
    }
    if (m_threads > 1) {
        return m_threads;
    }
    if (m_ids.size() < kParallelThreshold) {
        return 1;
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, std::min<unsigned>(hardware == 0 ? 1 : hardware, kMaxWorkers));
}

Status BruteForceIndex::Add(TrackletId id, std::span<const float> values) {
    if (m_dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "index dimension must be > 0");
    }
    if (values.size() != static_cast<std::size_t>(m_dim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "embedding dimension mismatch: index expects " + std::to_string(m_dim) +
                                 ", got " + std::to_string(values.size()));
    }
    if (!AreFinite(values)) {
        return Status::Error(ErrorCode::InvalidArgument, "embedding contains a non-finite value");
    }

    const auto existing = m_positions.find(id);
    if (existing != m_positions.end()) {
        std::copy(values.begin(), values.end(),
                  m_values.begin() + static_cast<std::ptrdiff_t>(existing->second * m_dim));
        return Status::Ok();
    }

    m_positions.emplace(id, m_ids.size());
    m_ids.push_back(id);
    m_values.insert(m_values.end(), values.begin(), values.end());
    return Status::Ok();
}

Status BruteForceIndex::Remove(TrackletId id) {
    const auto it = m_positions.find(id);
    if (it == m_positions.end()) {
        return Status::Error(ErrorCode::NotFound, "tracklet " + std::to_string(id) + " is not indexed");
    }

    const std::size_t position = it->second;
    const std::size_t last = m_ids.size() - 1;
    if (position != last) {
        const auto destination = m_values.begin() + static_cast<std::ptrdiff_t>(position * m_dim);
        const auto source = m_values.begin() + static_cast<std::ptrdiff_t>(last * m_dim);
        std::copy(source, source + m_dim, destination);
        m_ids[position] = m_ids[last];
        m_positions[m_ids[position]] = position;
    }
    m_ids.pop_back();
    m_values.resize(m_ids.size() * static_cast<std::size_t>(m_dim));
    m_positions.erase(it);
    return Status::Ok();
}

Status BruteForceIndex::Rebuild(std::span<const IndexEntry> entries) {
    if (m_dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "index dimension must be > 0");
    }

    std::vector<float> values;
    std::vector<TrackletId> ids;
    std::unordered_map<TrackletId, std::size_t> positions;
    values.reserve(entries.size() * static_cast<std::size_t>(m_dim));
    ids.reserve(entries.size());
    positions.reserve(entries.size());

    for (const IndexEntry& entry : entries) {
        if (entry.values.size() != static_cast<std::size_t>(m_dim)) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "rebuild entry " + std::to_string(entry.id) + " has dimension " +
                                     std::to_string(entry.values.size()) + ", expected " +
                                     std::to_string(m_dim));
        }
        if (!AreFinite(entry.values)) {
            return Status::Error(ErrorCode::InvalidArgument,
                                 "rebuild entry " + std::to_string(entry.id) +
                                     " contains a non-finite value");
        }

        const auto existing = positions.find(entry.id);
        if (existing != positions.end()) {
            std::copy(entry.values.begin(), entry.values.end(),
                      values.begin() + static_cast<std::ptrdiff_t>(existing->second * m_dim));
            continue;
        }
        positions.emplace(entry.id, ids.size());
        ids.push_back(entry.id);
        values.insert(values.end(), entry.values.begin(), entry.values.end());
    }

    // Commit only after the whole batch validated, so a bad entry leaves the
    // previous content intact.
    m_values = std::move(values);
    m_ids = std::move(ids);
    m_positions = std::move(positions);
    return Status::Ok();
}

Result<std::vector<SearchHit>> BruteForceIndex::Search(std::span<const float> query,
                                                       std::size_t k) const {
    return SearchImpl(query, k, nullptr);
}

Result<std::vector<SearchHit>> BruteForceIndex::Search(std::span<const float> query, std::size_t k,
                                                       const CandidateFilter& filter) const {
    return SearchImpl(query, k, &filter);
}

Result<std::vector<SearchHit>> BruteForceIndex::SearchImpl(std::span<const float> query, std::size_t k,
                                                           const CandidateFilter* filter) const {
    if (k == 0) {
        return std::vector<SearchHit>{};
    }
    if (m_dim <= 0) {
        return Status::Error(ErrorCode::InvalidArgument, "index dimension must be > 0");
    }
    if (query.size() != static_cast<std::size_t>(m_dim)) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "query dimension mismatch: index expects " + std::to_string(m_dim) +
                                 ", got " + std::to_string(query.size()));
    }
    if (!AreFinite(query)) {
        return Status::Error(ErrorCode::InvalidArgument, "query contains a non-finite value");
    }
    if (m_ids.empty()) {
        return std::vector<SearchHit>{};
    }

    const auto rows = static_cast<Eigen::Index>(m_ids.size());
    const Eigen::Map<const Eigen::VectorXf> vector(query.data(), m_dim);
    const Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        matrix(m_values.data(), rows, m_dim);
    Eigen::VectorXf scores(rows);

    const std::size_t workers = WorkerCount();
    if (workers <= 1) {
        scores.noalias() = matrix * vector;
    } else {
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            const auto begin = static_cast<Eigen::Index>(m_ids.size() * worker / workers);
            const auto end = static_cast<Eigen::Index>(m_ids.size() * (worker + 1) / workers);
            pool.emplace_back([&scores, &matrix, &vector, begin, end]() {
                scores.segment(begin, end - begin).noalias() =
                    matrix.middleRows(begin, end - begin) * vector;
            });
        }
        for (std::thread& thread : pool) {
            thread.join();
        }
    }

    std::vector<std::size_t> candidates;
    candidates.reserve(m_ids.size());
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        if (filter == nullptr || (*filter)(m_ids[i])) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty()) {
        return std::vector<SearchHit>{};
    }

    const std::size_t take = std::min(k, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(take),
                      candidates.end(), [&scores, this](std::size_t lhs, std::size_t rhs) {
                          const float left = scores(static_cast<Eigen::Index>(lhs));
                          const float right = scores(static_cast<Eigen::Index>(rhs));
                          if (left != right) {
                              return left > right;
                          }
                          return m_ids[lhs] < m_ids[rhs];
                      });

    std::vector<SearchHit> hits;
    hits.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        const std::size_t position = candidates[i];
        hits.push_back(SearchHit{m_ids[position], scores(static_cast<Eigen::Index>(position))});
    }
    return hits;
}

} // namespace tmcmt::reid
