#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "trafficmcmt/reid/BruteForceIndex.h"
#include "trafficmcmt/reid/EmbeddingIndex.h"

namespace {

using tmcmt::ErrorCode;
using tmcmt::IndexKind;
using tmcmt::TrackletId;
using tmcmt::reid::BruteForceIndex;
using tmcmt::reid::CreateEmbeddingIndex;
using tmcmt::reid::EmbeddingIndexOptions;
using tmcmt::reid::IndexEntry;
using tmcmt::reid::IsFaissAvailable;
using tmcmt::reid::SearchHit;

std::vector<IndexEntry> MakeEntries(
    const std::vector<std::pair<TrackletId, std::vector<float>>>& data) {
    std::vector<IndexEntry> entries;
    entries.reserve(data.size());
    for (const auto& [id, values] : data) {
        entries.push_back(IndexEntry{id, values});
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

TEST(BruteForceIndex, ReturnsTopKByInnerProduct) {
    BruteForceIndex index(3);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f, 0.f}));
    ASSERT_TRUE(index.Add(2, std::vector<float>{0.9f, 0.1f, 0.f}));
    ASSERT_TRUE(index.Add(3, std::vector<float>{0.f, 1.f, 0.f}));
    ASSERT_TRUE(index.Add(4, std::vector<float>{-1.f, 0.f, 0.f}));

    const auto hits = index.Search(std::vector<float>{1.f, 0.f, 0.f}, 3);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 3U);
    EXPECT_EQ(Ids(hits.value()), (std::vector<TrackletId>{1, 2, 3}));
    EXPECT_NEAR(hits.value()[0].score, 1.f, 1e-6f);
    EXPECT_NEAR(hits.value()[1].score, 0.9f, 1e-6f);
}

TEST(BruteForceIndex, EmptyIndexReturnsEmptyResult) {
    BruteForceIndex index(4);
    const auto hits = index.Search(std::vector<float>(4, 0.5f), 10);
    ASSERT_TRUE(hits) << hits.status().ToString();
    EXPECT_TRUE(hits.value().empty());
}

TEST(BruteForceIndex, KGreaterThanSizeReturnsAll) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(7, std::vector<float>{1.f, 0.f}));
    ASSERT_TRUE(index.Add(8, std::vector<float>{0.f, 1.f}));

    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 10);
    ASSERT_TRUE(hits) << hits.status().ToString();
    EXPECT_EQ(hits.value().size(), 2U);
}

TEST(BruteForceIndex, RejectsWrongDimensions) {
    BruteForceIndex index(3);
    EXPECT_EQ(index.Add(1, std::vector<float>{1.f, 2.f}).code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(index.Search(std::vector<float>{1.f, 2.f}, 1).status().code(),
              ErrorCode::InvalidArgument);
}

TEST(BruteForceIndex, RejectsNonFiniteValues) {
    BruteForceIndex index(2);
    const float infinity = std::numeric_limits<float>::infinity();
    EXPECT_EQ(index.Add(1, std::vector<float>{1.f, infinity}).code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(index.Search(std::vector<float>{1.f, infinity}, 1).status().code(),
              ErrorCode::InvalidArgument);
}

TEST(BruteForceIndex, RemoveUnknownReturnsNotFound) {
    BruteForceIndex index(2);
    EXPECT_EQ(index.Remove(42).code(), ErrorCode::NotFound);
}

TEST(BruteForceIndex, RemoveKeepsRemainingScores) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f}));
    ASSERT_TRUE(index.Add(2, std::vector<float>{0.5f, 0.f}));
    ASSERT_TRUE(index.Add(3, std::vector<float>{0.f, 1.f}));

    ASSERT_TRUE(index.Remove(2));
    EXPECT_EQ(index.Size(), 2U);

    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 2);
    ASSERT_TRUE(hits) << hits.status().ToString();
    EXPECT_EQ(Ids(hits.value()), (std::vector<TrackletId>{1, 3}));
}

TEST(BruteForceIndex, AddReplacesExistingId) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f}));
    ASSERT_TRUE(index.Add(2, std::vector<float>{0.f, 1.f}));
    ASSERT_TRUE(index.Add(1, std::vector<float>{0.f, 5.f}));

    EXPECT_EQ(index.Size(), 2U);
    const auto hits = index.Search(std::vector<float>{0.f, 1.f}, 1);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 1U);
}

TEST(BruteForceIndex, RebuildReplacesContent) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f}));

    const std::vector<std::pair<TrackletId, std::vector<float>>> data{
        {10, {0.f, 1.f}},
        {11, {1.f, 0.f}},
    };
    const auto entries = MakeEntries(data);
    ASSERT_TRUE(index.Rebuild(entries));
    EXPECT_EQ(index.Size(), 2U);

    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 1);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 11U);
}

TEST(BruteForceIndex, RebuildIsAtomicOnInvalidEntry) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f}));

    std::vector<float> good{0.f, 1.f};
    std::vector<float> bad{1.f};
    const std::vector<IndexEntry> entries{IndexEntry{10, good}, IndexEntry{11, bad}};
    EXPECT_EQ(index.Rebuild(entries).code(), ErrorCode::InvalidArgument);

    // The old content is still searchable.
    EXPECT_EQ(index.Size(), 1U);
    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 1);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 1U);
}

TEST(BruteForceIndex, RebuildReplacesDuplicateIds) {
    BruteForceIndex index(2);
    std::vector<float> first{1.f, 0.f};
    std::vector<float> second{0.f, 1.f};
    const std::vector<IndexEntry> entries{IndexEntry{5, first}, IndexEntry{5, second}};
    ASSERT_TRUE(index.Rebuild(entries));
    EXPECT_EQ(index.Size(), 1U);

    const auto hits = index.Search(std::vector<float>{0.f, 1.f}, 1);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 5U);
}

TEST(BruteForceIndex, TiesBreakById) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(5, std::vector<float>{1.f, 0.f}));
    ASSERT_TRUE(index.Add(3, std::vector<float>{1.f, 0.f}));

    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 2);
    ASSERT_TRUE(hits) << hits.status().ToString();
    EXPECT_EQ(Ids(hits.value()), (std::vector<TrackletId>{3, 5}));
}

TEST(BruteForceIndex, ThreadedScanMatchesSingleThread) {
    BruteForceIndex single(8, 1);
    BruteForceIndex threaded(8, 4);
    for (TrackletId id = 1; id <= 64; ++id) {
        std::vector<float> values(8);
        for (std::size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<float>((id * (i + 3)) % 17) - 8.f;
        }
        ASSERT_TRUE(single.Add(id, values));
        ASSERT_TRUE(threaded.Add(id, values));
    }

    const std::vector<float> query{1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f};
    const auto expected = single.Search(query, 5);
    const auto actual = threaded.Search(query, 5);
    ASSERT_TRUE(expected) << expected.status().ToString();
    ASSERT_TRUE(actual) << actual.status().ToString();
    EXPECT_EQ(Ids(expected.value()), Ids(actual.value()));
}

TEST(BruteForceIndex, FilterIsAppliedBeforeTopK) {
    BruteForceIndex index(2);
    ASSERT_TRUE(index.Add(1, std::vector<float>{1.f, 0.f}));
    ASSERT_TRUE(index.Add(2, std::vector<float>{0.9f, 0.f}));
    ASSERT_TRUE(index.Add(3, std::vector<float>{0.8f, 0.f}));

    const auto hits = index.Search(std::vector<float>{1.f, 0.f}, 1,
                                   [](TrackletId id) { return id != 1; });
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 2U);
}

TEST(EmbeddingIndexFactory, CreatesBruteForceIndex) {
    EmbeddingIndexOptions options;
    options.dim = 4;
    auto index = CreateEmbeddingIndex(IndexKind::Brute, options);
    ASSERT_TRUE(index) << index.status().ToString();
    EXPECT_EQ(index.value()->Kind(), IndexKind::Brute);
    EXPECT_EQ(index.value()->Dim(), 4);
    EXPECT_EQ(index.value()->Size(), 0U);
}

TEST(EmbeddingIndexFactory, RejectsInvalidDimension) {
    EmbeddingIndexOptions options;
    options.dim = 0;
    auto index = CreateEmbeddingIndex(IndexKind::Brute, options);
    ASSERT_FALSE(index);
    EXPECT_EQ(index.status().code(), ErrorCode::InvalidArgument);
}

TEST(EmbeddingIndexFactory, ReportsFaissAvailability) {
    EmbeddingIndexOptions options;
    options.dim = 4;
    auto index = CreateEmbeddingIndex(IndexKind::FaissFlat, options);
    if (IsFaissAvailable()) {
        ASSERT_TRUE(index) << index.status().ToString();
        EXPECT_EQ(index.value()->Kind(), IndexKind::FaissFlat);
    } else {
        ASSERT_FALSE(index);
        EXPECT_EQ(index.status().code(), ErrorCode::Unsupported);
        EXPECT_NE(index.status().message().find("TRAFFICMCMT_WITH_FAISS"), std::string::npos);
    }
}

} // namespace
