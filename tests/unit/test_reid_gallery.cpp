#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/reid/EmbeddingNormalizer.h"
#include "trafficmcmt/reid/GalleryManager.h"
#include "trafficmcmt/reid/ReIdMatcher.h"

namespace {

using tmcmt::CameraId;
using tmcmt::Embedding;
using tmcmt::ErrorCode;
using tmcmt::IndexKind;
using tmcmt::ObjectType;
using tmcmt::Timestamp;
using tmcmt::Tracklet;
using tmcmt::TrackletId;
using tmcmt::reid::EmbeddingNormalizer;
using tmcmt::reid::GalleryEntry;
using tmcmt::reid::GalleryManager;
using tmcmt::reid::GalleryOptions;
using tmcmt::reid::GalleryQuery;
using tmcmt::reid::MatcherOptions;
using tmcmt::reid::ReIdMatcher;

Timestamp Ms(std::int64_t milliseconds) {
    return Timestamp(milliseconds);
}

class TempFile {
public:
    explicit TempFile(const std::string& name) {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path() /
                 ("tmcmt_reid_" + std::to_string(stamp) + "_" + std::to_string(counter++) + "_" + name);
        m_pathString = m_path.string();
    }

    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    [[nodiscard]] const std::string& Path() const {
        return m_pathString;
    }

private:
    std::filesystem::path m_path;
    std::string m_pathString;
};

GalleryEntry Entry(TrackletId id, const CameraId& camera, std::vector<float> values,
                   std::uint32_t model_id = 1, ObjectType type = ObjectType::Car,
                   std::int64_t begin = 0, std::int64_t end = 1000) {
    GalleryEntry entry;
    entry.id = id;
    entry.camera = camera;
    entry.values = std::move(values);
    entry.model_id = model_id;
    entry.type = type;
    entry.begin = Ms(begin);
    entry.end = Ms(end);
    return entry;
}

std::unique_ptr<GalleryManager> MakeGallery(int dim = 2) {
    GalleryOptions options;
    options.index.dim = dim;
    auto gallery = GalleryManager::Create(IndexKind::Brute, options);
    if (!gallery) {
        return nullptr;
    }
    return std::move(gallery).value();
}

TEST(EmbeddingNormalizer, IdentityProducesUnitVector) {
    auto normalizer = EmbeddingNormalizer::Identity(2);
    ASSERT_TRUE(normalizer) << normalizer.status().ToString();

    const auto values = normalizer.value().Apply(std::vector<float>{3.f, 4.f});
    ASSERT_TRUE(values) << values.status().ToString();
    ASSERT_EQ(values.value().size(), 2U);
    EXPECT_NEAR(values.value()[0], 0.6f, 1e-6f);
    EXPECT_NEAR(values.value()[1], 0.8f, 1e-6f);
}

TEST(EmbeddingNormalizer, AppliesWhiteningBeforeNormalisation) {
    auto normalizer = EmbeddingNormalizer::FromWhitening(
        std::vector<float>{1.f, 2.f},
        std::vector<float>{1.f, 0.f, 0.f, 2.f}, 2);
    ASSERT_TRUE(normalizer) << normalizer.status().ToString();

    const auto values = normalizer.value().Apply(std::vector<float>{3.f, 4.f});
    ASSERT_TRUE(values) << values.status().ToString();
    ASSERT_EQ(values.value().size(), 2U);
    const float norm = std::sqrt(values.value()[0] * values.value()[0] +
                                 values.value()[1] * values.value()[1]);
    EXPECT_NEAR(norm, 1.f, 1e-5f);
    EXPECT_NEAR(values.value()[0] / values.value()[1], 0.5f, 1e-5f);
}

TEST(EmbeddingNormalizer, RejectsZeroVector) {
    auto normalizer = EmbeddingNormalizer::Identity(2);
    ASSERT_TRUE(normalizer);
    const auto values = normalizer.value().Apply(std::vector<float>{0.f, 0.f});
    ASSERT_FALSE(values);
    EXPECT_EQ(values.status().code(), ErrorCode::InvalidArgument);
}

TEST(EmbeddingNormalizer, RejectsWrongDimension) {
    auto normalizer = EmbeddingNormalizer::Identity(3);
    ASSERT_TRUE(normalizer);
    const auto values = normalizer.value().Apply(std::vector<float>{1.f, 2.f});
    ASSERT_FALSE(values);
    EXPECT_EQ(values.status().code(), ErrorCode::InvalidArgument);
}

TEST(EmbeddingNormalizer, SaveLoadRoundTrip) {
    auto normalizer = EmbeddingNormalizer::FromWhitening(
        std::vector<float>{0.5f, -1.f},
        std::vector<float>{2.f, 0.f, 0.f, 0.5f}, 2);
    ASSERT_TRUE(normalizer) << normalizer.status().ToString();

    const TempFile file("whitening.bin");
    ASSERT_TRUE(normalizer.value().Save(file.Path()));

    auto loaded = EmbeddingNormalizer::Load(file.Path());
    ASSERT_TRUE(loaded) << loaded.status().ToString();
    EXPECT_EQ(loaded.value().InDim(), 2);
    EXPECT_EQ(loaded.value().OutDim(), 2);

    const std::vector<float> input{1.f, 3.f};
    const auto expected = normalizer.value().Apply(input);
    const auto actual = loaded.value().Apply(input);
    ASSERT_TRUE(expected);
    ASSERT_TRUE(actual);
    ASSERT_EQ(actual.value().size(), expected.value().size());
    for (std::size_t i = 0; i < expected.value().size(); ++i) {
        EXPECT_NEAR(actual.value()[i], expected.value()[i], 1e-6f);
    }
}

TEST(EmbeddingNormalizer, RejectsMissingArtifact) {
    auto loaded = EmbeddingNormalizer::Load("definitely_missing_whitening.bin");
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), ErrorCode::IoError);
}

TEST(EmbeddingNormalizer, RejectsBrokenArtifact) {
    const TempFile file("broken.bin");
    {
        std::ofstream out(file.Path(), std::ios::binary | std::ios::trunc);
        out << "not-a-whitening-artifact";
    }
    auto loaded = EmbeddingNormalizer::Load(file.Path());
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.status().code(), ErrorCode::ParseError);
}

TEST(EmbeddingNormalizer, ApplyKeepsModelId) {
    auto normalizer = EmbeddingNormalizer::Identity(2);
    ASSERT_TRUE(normalizer);
    Embedding input;
    input.values = {3.f, 4.f};
    input.dim = 2;
    input.model_id = 7;

    const auto output = normalizer.value().Apply(input);
    ASSERT_TRUE(output) << output.status().ToString();
    EXPECT_EQ(output.value().model_id, 7U);
    EXPECT_EQ(output.value().dim, 2U);
}

TEST(GalleryManager, AddsTrackletAndIndexesEmbedding) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);

    Tracklet tracklet;
    tracklet.id = 11;
    tracklet.session_id = "session";
    tracklet.camera = "cam-a";
    tracklet.local_track_id = 1;
    tracklet.begin = Ms(0);
    tracklet.end = Ms(1000);
    tracklet.type = ObjectType::Car;
    tracklet.embedding.values = {1.f, 0.f};
    tracklet.embedding.dim = 2;
    tracklet.embedding.model_id = 3;

    ASSERT_TRUE(gallery->Add(tracklet));
    EXPECT_EQ(gallery->Size(), 1U);
    ASSERT_NE(gallery->Get(11), nullptr);
    EXPECT_EQ(gallery->Get(11)->model_id, 3U);
    EXPECT_EQ(gallery->Get(11)->type, ObjectType::Car);
}

TEST(GalleryManager, RejectsInvalidEntries) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);

    EXPECT_EQ(gallery->Add(Entry(1, "cam-a", {1.f, 0.f, 0.f})).code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(gallery->Add(Entry(2, "", {1.f, 0.f})).code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(gallery->Remove(99).code(), ErrorCode::NotFound);
}

TEST(GalleryManager, ExcludesSameCameraByDefault) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-a", {1.f, 0.f})));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.9f, 0.1f})));
    ASSERT_TRUE(gallery->Add(Entry(3, "cam-c", {0.f, 1.f})));

    GalleryQuery query;
    query.top_k = 10;
    query.query_camera = "cam-a";

    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 2U);
    EXPECT_EQ(hits.value()[0].id, 2U);
    EXPECT_EQ(hits.value()[1].id, 3U);
}

TEST(GalleryManager, CanIncludeSameCamera) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-a", {1.f, 0.f})));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.9f, 0.1f})));

    GalleryQuery query;
    query.top_k = 10;
    query.query_camera = "cam-a";
    query.exclude_same_camera = false;

    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_FALSE(hits.value().empty());
    EXPECT_EQ(hits.value()[0].id, 1U);
}

TEST(GalleryManager, ExcludesQueryId) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {1.f, 0.f})));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.9f, 0.1f})));

    GalleryQuery query;
    query.top_k = 10;
    query.exclude_id = 1;

    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 2U);
}

TEST(GalleryManager, FiltersByModelTypeCameraAndTime) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {1.f, 0.f}, 1, ObjectType::Car, 0, 1000)));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-c", {1.f, 0.f}, 2, ObjectType::Car, 0, 1000)));
    ASSERT_TRUE(gallery->Add(Entry(3, "cam-b", {1.f, 0.f}, 1, ObjectType::Pedestrian, 0, 1000)));
    ASSERT_TRUE(gallery->Add(Entry(4, "cam-b", {1.f, 0.f}, 1, ObjectType::Car, 5000, 6000)));

    GalleryQuery query;
    query.top_k = 10;
    query.model_id = 1;
    query.type = ObjectType::Car;
    query.camera = CameraId{"cam-b"};
    query.from = Ms(1500);

    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 4U);
}

TEST(GalleryManager, AppliesMinimumSimilarity) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {1.f, 0.f})));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.f, 1.f})));

    GalleryQuery query;
    query.top_k = 10;
    query.min_similarity = 0.5f;

    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 1U);
}

TEST(GalleryManager, EvictsEntriesOlderThanRetention) {
    GalleryOptions options;
    options.index.dim = 2;
    options.retention_ms = 500;
    auto gallery = GalleryManager::Create(IndexKind::Brute, options);
    ASSERT_TRUE(gallery) << gallery.status().ToString();

    ASSERT_TRUE(gallery.value()->Add(Entry(1, "cam-a", {1.f, 0.f}, 1, ObjectType::Car, 0, 1000)));
    ASSERT_TRUE(gallery.value()->Add(Entry(2, "cam-a", {0.f, 1.f}, 1, ObjectType::Car, 1000, 1900)));

    EXPECT_EQ(gallery.value()->Evict(Ms(2000)), 1U);
    EXPECT_EQ(gallery.value()->Size(), 1U);
    EXPECT_EQ(gallery.value()->Get(1), nullptr);
    ASSERT_NE(gallery.value()->Get(2), nullptr);
}

TEST(GalleryManager, ReAddUpdatesMetadata) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-a", {1.f, 0.f}, 1, ObjectType::Car)));
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {0.f, 1.f}, 2, ObjectType::Pedestrian)));

    EXPECT_EQ(gallery->Size(), 1U);
    const auto cameras = gallery->Cameras();
    ASSERT_EQ(cameras.size(), 1U);
    EXPECT_EQ(cameras[0], "cam-b");
    ASSERT_NE(gallery->Get(1), nullptr);
    EXPECT_EQ(gallery->Get(1)->model_id, 2U);
    EXPECT_EQ(gallery->Get(1)->type, ObjectType::Pedestrian);
}

TEST(GalleryManager, RemoveDropsEntryFromSearch) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {1.f, 0.f})));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.5f, 0.f})));

    ASSERT_TRUE(gallery->Remove(1));
    EXPECT_EQ(gallery->Size(), 1U);

    GalleryQuery query;
    query.top_k = 10;
    const auto hits = gallery->Search(std::vector<float>{1.f, 0.f}, query);
    ASSERT_TRUE(hits) << hits.status().ToString();
    ASSERT_EQ(hits.value().size(), 1U);
    EXPECT_EQ(hits.value()[0].id, 2U);
}

TEST(ReIdMatcher, ReturnsCandidatesWithAppearanceCost) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-a", {1.f, 0.f}, 1)));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {0.8f, 0.6f}, 1)));
    ASSERT_TRUE(gallery->Add(Entry(3, "cam-c", {0.f, 1.f}, 1)));

    MatcherOptions options;
    options.top_k = 2;
    const ReIdMatcher matcher(*gallery, options);

    const auto candidates = matcher.Match(1, "cam-a", std::vector<float>{1.f, 0.f}, 1);
    ASSERT_TRUE(candidates) << candidates.status().ToString();
    ASSERT_EQ(candidates.value().size(), 2U);
    EXPECT_EQ(candidates.value()[0].candidate_id, 2U);
    EXPECT_NEAR(candidates.value()[0].similarity, 0.8f, 1e-6f);
    EXPECT_NEAR(candidates.value()[0].cost, 0.2f, 1e-6f);
    EXPECT_EQ(candidates.value()[1].candidate_id, 3U);
}

TEST(ReIdMatcher, NeverComparesDifferentModels) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    ASSERT_TRUE(gallery->Add(Entry(1, "cam-b", {1.f, 0.f}, 1)));
    ASSERT_TRUE(gallery->Add(Entry(2, "cam-b", {1.f, 0.f}, 2)));

    const ReIdMatcher matcher(*gallery, MatcherOptions{});
    const auto candidates = matcher.Match(99, "cam-a", std::vector<float>{1.f, 0.f}, 2);
    ASSERT_TRUE(candidates) << candidates.status().ToString();
    ASSERT_EQ(candidates.value().size(), 1U);
    EXPECT_EQ(candidates.value()[0].candidate_id, 2U);
}

TEST(ReIdMatcher, RejectsInvalidEmbedding) {
    auto gallery = MakeGallery();
    ASSERT_NE(gallery, nullptr);
    const ReIdMatcher matcher(*gallery, MatcherOptions{});

    Tracklet query;
    query.id = 5;
    const auto candidates = matcher.Match(query);
    ASSERT_FALSE(candidates);
    EXPECT_EQ(candidates.status().code(), ErrorCode::InvalidArgument);
}

} // namespace
