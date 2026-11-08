#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

#include "trafficmcmt/reid/BruteForceIndex.h"
#include "trafficmcmt/reid/EmbeddingIndex.h"
#include "trafficmcmt/reid/FaissIndex.h"

#if defined(TMCMT_WITH_FAISS)

namespace {

using tmcmt::IndexKind;
using tmcmt::TrackletId;
using tmcmt::reid::BruteForceIndex;
using tmcmt::reid::CreateEmbeddingIndex;
using tmcmt::reid::EmbeddingIndexOptions;
using tmcmt::reid::IndexEntry;
using tmcmt::reid::SearchHit;

std::vector<std::vector<float>> MakeVectors(std::size_t count, int dim, std::uint32_t seed) {
    std::mt19937 engine(seed);
    std::normal_distribution<float> distribution(0.f, 1.f);
    std::vector<std::vector<float>> vectors(count, std::vector<float>(dim));
    for (auto& values : vectors) {
        float squared = 0.f;
        for (float& value : values) {
            value = distribution(engine);
            squared += value * value;
        }
        const float inverse = 1.f / std::sqrt(squared);
        for (float& value : values) {
            value *= inverse;
        }
    }
    return vectors;
}

std::vector<IndexEntry> MakeEntries(const std::vector<std::vector<float>>& vectors) {
    std::vector<IndexEntry> entries;
    entries.reserve(vectors.size());
    for (std::size_t i = 0; i < vectors.size(); ++i) {
        entries.push_back(IndexEntry{static_cast<TrackletId>(i + 1), vectors[i]});
    }
    return entries;
}

std::vector<TrackletId> Ids(const std::vector<SearchHit>& hits) {
    std::vector<TrackletId> ids;
    ids.reserve(hits.size());
    for (const SearchHit& hit : hits) {
        ids.push_back(hit.id);
    }
    return ids;
}

EmbeddingIndexOptions Options(int dim) {
    EmbeddingIndexOptions options;
    options.dim = dim;
    return options;
}

TEST(FaissIndex, FlatMatchesBruteForceTopK) {
    constexpr int kDim = 32;
    constexpr std::size_t kCount = 2000;
    const auto vectors = MakeVectors(kCount, kDim, 7);
    const auto entries = MakeEntries(vectors);

    BruteForceIndex brute(Options(kDim));
    ASSERT_TRUE(brute.Rebuild(entries));

    auto flat = CreateEmbeddingIndex(IndexKind::FaissFlat, Options(kDim));
    ASSERT_TRUE(flat) << flat.status().ToString();
    ASSERT_TRUE(flat.value()->Rebuild(entries));

    const auto queries = MakeVectors(20, kDim, 99);
    for (const auto& query : queries) {
        const auto expected = brute.Search(query, 10);
        const auto actual = flat.value()->Search(query, 10);
        ASSERT_TRUE(expected) << expected.status().ToString();
        ASSERT_TRUE(actual) << actual.status().ToString();
        EXPECT_EQ(Ids(expected.value()), Ids(actual.value()));
    }
}

TEST(FaissIndex, IvfPqRecallIsHigh) {
    constexpr int kDim = 32;
    constexpr std::size_t kCount = 5000;
    const auto vectors = MakeVectors(kCount, kDim, 11);
    const auto entries = MakeEntries(vectors);

    BruteForceIndex brute(Options(kDim));
    ASSERT_TRUE(brute.Rebuild(entries));

    EmbeddingIndexOptions options = Options(kDim);
    options.nlist = 64;
    options.nprobe = 16;
    options.pq_m = 8;
    auto ivf = CreateEmbeddingIndex(IndexKind::FaissIvfPq, options);
    ASSERT_TRUE(ivf) << ivf.status().ToString();
    ASSERT_TRUE(ivf.value()->Rebuild(entries));

    const auto queries = MakeVectors(50, kDim, 123);
    std::size_t matched = 0;
    std::size_t total = 0;
    for (const auto& query : queries) {
        const auto expected = brute.Search(query, 10);
        const auto actual = ivf.value()->Search(query, 10);
        ASSERT_TRUE(expected) << expected.status().ToString();
        ASSERT_TRUE(actual) << actual.status().ToString();
        const auto expectedIds = Ids(expected.value());
        const std::set<TrackletId> reference(expectedIds.begin(), expectedIds.end());
        for (const TrackletId id : Ids(actual.value())) {
            if (reference.count(id) != 0) {
                ++matched;
            }
        }
        total += reference.size();
    }
    const double recall = static_cast<double>(matched) / static_cast<double>(total);
    EXPECT_GE(recall, 0.90);
}

TEST(FaissIndex, HnswReturnsNeighbours) {
    constexpr int kDim = 16;
    const auto vectors = MakeVectors(500, kDim, 5);
    const auto entries = MakeEntries(vectors);

    EmbeddingIndexOptions options = Options(kDim);
    options.hnsw_m = 16;
    auto hnsw = CreateEmbeddingIndex(IndexKind::FaissHnsw, options);
    ASSERT_TRUE(hnsw) << hnsw.status().ToString();
    ASSERT_TRUE(hnsw.value()->Rebuild(entries));

    const auto hits = hnsw.value()->Search(vectors.front(), 5);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 5U);
    EXPECT_EQ(hits.value()[0].id, 1U);
}

TEST(FaissIndex, RemoveAndAddKeepSizeConsistent) {
    constexpr int kDim = 8;
    const auto vectors = MakeVectors(100, kDim, 3);
    const auto entries = MakeEntries(vectors);

    auto index = CreateEmbeddingIndex(IndexKind::FaissFlat, Options(kDim));
    ASSERT_TRUE(index) << index.status().ToString();
    ASSERT_TRUE(index.value()->Rebuild(entries));
    EXPECT_EQ(index.value()->Size(), 100U);

    ASSERT_TRUE(index.value()->Remove(10));
    EXPECT_EQ(index.value()->Size(), 99U);
    EXPECT_EQ(index.value()->Remove(10).code(), tmcmt::ErrorCode::NotFound);

    ASSERT_TRUE(index.value()->Add(1000, vectors.front()));
    EXPECT_EQ(index.value()->Size(), 100U);
}

} // namespace

#endif
