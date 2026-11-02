#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "trafficmcmt/edge/EdgeTrackExporter.h"

namespace {

using tmcmt::EdgeExportConfig;
using tmcmt::EdgeTrackExporter;
using tmcmt::Embedding;
using tmcmt::Observation;
using tmcmt::Timestamp;

Embedding MakeEmbedding(std::uint32_t model_id = 1) {
    Embedding embedding;
    embedding.values = {1.f, 0.f, 0.f, 0.f};
    embedding.dim = 4;
    embedding.model_id = model_id;
    return embedding;
}

Observation MakeObservation(std::int64_t ts_ms) {
    Observation observation;
    observation.ts = Timestamp(ts_ms);
    observation.bbox = tmcmt::BBox{10.f, 10.f, 20.f, 20.f, 0.9f};
    return observation;
}

EdgeExportConfig MakeConfig() {
    EdgeExportConfig config;
    config.camera_id = "cam-01";
    config.session_id = "session-01";
    config.chunk_interval_ms = 1000;
    config.min_observations = 2;
    config.model_id = 1;
    return config;
}

TEST(EdgeTrackExporter, EmitsPeriodicChunks) {
    EdgeTrackExporter exporter(MakeConfig());
    const Embedding embedding = MakeEmbedding();

    EXPECT_TRUE(exporter.AddObservation(42, MakeObservation(0), embedding, tmcmt::ObjectType::Car, 0.9f,
                                        Timestamp(0))
                    .empty());
    EXPECT_TRUE(exporter.AddObservation(42, MakeObservation(100), embedding, tmcmt::ObjectType::Car, 0.9f,
                                        Timestamp(100))
                    .empty());

    std::vector<tmcmt::Tracklet> chunks =
        exporter.AddObservation(42, MakeObservation(1000), embedding, tmcmt::ObjectType::Car, 0.9f, Timestamp(1000));
    ASSERT_EQ(chunks.size(), 1U);

    const tmcmt::Tracklet& chunk = chunks.front();
    EXPECT_EQ(chunk.session_id, "session-01");
    EXPECT_EQ(chunk.camera, "cam-01");
    EXPECT_EQ(chunk.local_track_id, 42U);
    EXPECT_EQ(chunk.sequence, 0U);
    EXPECT_FALSE(chunk.is_end);
    EXPECT_EQ(chunk.type, tmcmt::ObjectType::Car);
    ASSERT_EQ(chunk.observations.size(), 3U);
    EXPECT_EQ(chunk.embedding.dim, 4U);
    EXPECT_NEAR(chunk.embedding.Norm(), 1.f, 1e-5f);
    EXPECT_GE(chunk.quality, 0.f);
    EXPECT_LE(chunk.quality, 1.f);
    EXPECT_NE(chunk.id, 0U);

    EXPECT_TRUE(exporter.AddObservation(42, MakeObservation(1100), embedding, tmcmt::ObjectType::Car, 0.9f,
                                        Timestamp(1100))
                    .empty());

    chunks = exporter.Flush();
    ASSERT_EQ(chunks.size(), 1U);
    EXPECT_TRUE(chunks.front().is_end);
    EXPECT_EQ(chunks.front().sequence, 1U);
    ASSERT_EQ(chunks.front().observations.size(), 1U);
    EXPECT_EQ(exporter.OpenTracks(), 0U);
}

TEST(EdgeTrackExporter, TickEmitsWithoutNewObservation) {
    EdgeExportConfig config = MakeConfig();
    config.chunk_interval_ms = 100;
    config.min_observations = 1;
    EdgeTrackExporter exporter(config);

    EXPECT_TRUE(exporter.AddObservation(1, MakeObservation(0), MakeEmbedding(), tmcmt::ObjectType::Pedestrian, 0.5f,
                                        Timestamp(0))
                    .empty());

    const std::vector<tmcmt::Tracklet> chunks = exporter.Tick(Timestamp(100));
    ASSERT_EQ(chunks.size(), 1U);
    EXPECT_EQ(chunks.front().sequence, 0U);
    EXPECT_EQ(chunks.front().observations.size(), 1U);
}

TEST(EdgeTrackExporter, DropsTracksWithoutEmbedding) {
    EdgeTrackExporter exporter(MakeConfig());
    const Embedding empty;

    EXPECT_TRUE(exporter.AddObservation(1, MakeObservation(0), empty, tmcmt::ObjectType::Unknown, 0.f, Timestamp(0))
                    .empty());
    EXPECT_TRUE(exporter.AddObservation(1, MakeObservation(1000), empty, tmcmt::ObjectType::Unknown, 0.f,
                                        Timestamp(1000))
                    .empty());
    EXPECT_TRUE(exporter.Flush().empty());
}

TEST(EdgeTrackExporter, TracksAreIndependent) {
    EdgeExportConfig config = MakeConfig();
    config.chunk_interval_ms = 100;
    config.min_observations = 1;
    EdgeTrackExporter exporter(config);

    const std::vector<tmcmt::Tracklet> first =
        exporter.AddObservation(1, MakeObservation(0), MakeEmbedding(), tmcmt::ObjectType::Car, 0.9f, Timestamp(100));
    const std::vector<tmcmt::Tracklet> second =
        exporter.AddObservation(2, MakeObservation(100), MakeEmbedding(), tmcmt::ObjectType::Truck, 0.8f,
                                Timestamp(200));

    ASSERT_EQ(first.size(), 1U);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(first.front().local_track_id, 1U);
    EXPECT_EQ(second.front().local_track_id, 2U);
    EXPECT_EQ(second.front().type, tmcmt::ObjectType::Truck);
    EXPECT_EQ(exporter.OpenTracks(), 2U);
}

TEST(EdgeTrackExporter, NeverMixesModels) {
    EdgeTrackExporter exporter(MakeConfig());

    EXPECT_TRUE(exporter
                    .AddObservation(1, MakeObservation(0), MakeEmbedding(1), tmcmt::ObjectType::Car, 0.9f,
                                    Timestamp(0))
                    .empty());
    EXPECT_TRUE(exporter
                    .AddObservation(1, MakeObservation(10), MakeEmbedding(2), tmcmt::ObjectType::Car, 0.9f,
                                    Timestamp(10))
                    .empty());

    const std::vector<tmcmt::Tracklet> chunks = exporter.Flush();
    ASSERT_EQ(chunks.size(), 1U);
    EXPECT_EQ(chunks.front().embedding.model_id, 1U);
}

} // namespace
