#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>

#include "tracklet.pb.h"
#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/io/TrackletCodec.h"

#include "TrackletTestHelpers.h"

namespace {

using tmcmt::CalibrationCodec;
using tmcmt::Tracklet;
using tmcmt::TrackletCodec;
using tmcmt::TrackletCodecOptions;

void ExpectTrackletEqual(const Tracklet& actual, const Tracklet& expected) {
    EXPECT_EQ(actual.session_id, expected.session_id);
    EXPECT_EQ(actual.camera, expected.camera);
    EXPECT_EQ(actual.local_track_id, expected.local_track_id);
    EXPECT_EQ(actual.sequence, expected.sequence);
    EXPECT_EQ(actual.begin, expected.begin);
    EXPECT_EQ(actual.end, expected.end);
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.is_end, expected.is_end);
    EXPECT_EQ(actual.schema_version, expected.schema_version);
    EXPECT_FLOAT_EQ(actual.quality, expected.quality);
    EXPECT_EQ(actual.embedding.dim, expected.embedding.dim);
    EXPECT_EQ(actual.embedding.model_id, expected.embedding.model_id);
    ASSERT_EQ(actual.embedding.values.size(), expected.embedding.values.size());
    for (std::size_t i = 0; i < expected.embedding.values.size(); ++i) {
        EXPECT_FLOAT_EQ(actual.embedding.values[i], expected.embedding.values[i]);
    }
    ASSERT_EQ(actual.observations.size(), expected.observations.size());
    for (std::size_t i = 0; i < expected.observations.size(); ++i) {
        EXPECT_EQ(actual.observations[i].ts, expected.observations[i].ts);
        EXPECT_FLOAT_EQ(actual.observations[i].bbox.x, expected.observations[i].bbox.x);
        EXPECT_FLOAT_EQ(actual.observations[i].bbox.y, expected.observations[i].bbox.y);
        EXPECT_FLOAT_EQ(actual.observations[i].bbox.w, expected.observations[i].bbox.w);
        EXPECT_FLOAT_EQ(actual.observations[i].bbox.h, expected.observations[i].bbox.h);
        EXPECT_FLOAT_EQ(actual.observations[i].bbox.score, expected.observations[i].bbox.score);
        EXPECT_EQ(actual.observations[i].world.valid, expected.observations[i].world.valid);
        EXPECT_FLOAT_EQ(actual.observations[i].world.x, expected.observations[i].world.x);
        EXPECT_FLOAT_EQ(actual.observations[i].world.y, expected.observations[i].world.y);
    }
}

TEST(TrackletCodec, BinaryRoundtrip) {
    const Tracklet input = tmcmt::test::MakeTracklet();
    auto encoded = TrackletCodec::EncodeBinary(input);
    ASSERT_TRUE(encoded) << encoded.status().ToString();

    auto decoded = TrackletCodec::DecodeBinary(encoded.value());
    ASSERT_TRUE(decoded) << decoded.status().ToString();
    ExpectTrackletEqual(decoded.value(), input);
}

TEST(TrackletCodec, FramedRoundtripReportsConsumedBytes) {
    Tracklet input = tmcmt::test::MakeTracklet();
    input.is_end = true;

    auto framed = TrackletCodec::EncodeFramed(input);
    ASSERT_TRUE(framed) << framed.status().ToString();

    std::size_t consumed = 0;
    auto decoded = TrackletCodec::DecodeFramed(framed.value(), &consumed);
    ASSERT_TRUE(decoded) << decoded.status().ToString();
    EXPECT_EQ(consumed, framed.value().size());
    EXPECT_TRUE(decoded.value().is_end);
    ExpectTrackletEqual(decoded.value(), input);
}

TEST(TrackletCodec, FramedRejectsTruncatedPayload) {
    auto framed = TrackletCodec::EncodeFramed(tmcmt::test::MakeTracklet());
    ASSERT_TRUE(framed);
    const std::string truncated = framed.value().substr(0, framed.value().size() / 2);

    auto decoded = TrackletCodec::DecodeFramed(truncated);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.status().code(), tmcmt::ErrorCode::ParseError);
}

TEST(TrackletCodec, JsonRoundtrip) {
    const Tracklet input = tmcmt::test::MakeTracklet();
    auto json = TrackletCodec::EncodeJson(input, true);
    ASSERT_TRUE(json) << json.status().ToString();

    auto decoded = TrackletCodec::DecodeJson(json.value());
    ASSERT_TRUE(decoded) << decoded.status().ToString();
    ExpectTrackletEqual(decoded.value(), input);
}

TEST(TrackletCodec, JsonRejectsUnknownFields) {
    auto json = TrackletCodec::EncodeJson(tmcmt::test::MakeTracklet());
    ASSERT_TRUE(json);
    const std::string corrupted = json.value().substr(0, json.value().size() - 1) + ",\"unexpected\":1}";

    auto decoded = TrackletCodec::DecodeJson(corrupted);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.status().code(), tmcmt::ErrorCode::ParseError);
}

TEST(TrackletCodec, Int8QuantizationIsLossyButClose) {
    Tracklet input = tmcmt::test::MakeTracklet();
    input.embedding.values = {0.5f, 0.25f, -0.5f, -0.25f};
    input.embedding.dim = 4;

    TrackletCodecOptions options;
    options.quantize_embeddings = true;

    auto encoded = TrackletCodec::EncodeBinary(input, options);
    ASSERT_TRUE(encoded) << encoded.status().ToString();

    auto decoded = TrackletCodec::DecodeBinary(encoded.value());
    ASSERT_TRUE(decoded) << decoded.status().ToString();
    ASSERT_EQ(decoded.value().embedding.values.size(), input.embedding.values.size());
    for (std::size_t i = 0; i < input.embedding.values.size(); ++i) {
        EXPECT_NEAR(decoded.value().embedding.values[i], input.embedding.values[i], 0.01f);
    }
    EXPECT_LE(encoded.value().size(), TrackletCodec::EncodeBinary(input).value().size());
}

TEST(TrackletCodec, RejectsUnsupportedSchemaVersion) {
    auto encoded = TrackletCodec::EncodeBinary(tmcmt::test::MakeTracklet());
    ASSERT_TRUE(encoded);

    tmcmt::v1::TrackletChunk chunk;
    ASSERT_TRUE(chunk.ParseFromString(encoded.value()));
    chunk.set_schema_version(2);

    std::string payload;
    ASSERT_TRUE(chunk.SerializeToString(&payload));

    auto decoded = TrackletCodec::DecodeBinary(payload);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.status().code(), tmcmt::ErrorCode::Unsupported);
}

TEST(TrackletCodec, RejectsEmbeddingDimMismatch) {
    auto encoded = TrackletCodec::EncodeBinary(tmcmt::test::MakeTracklet());
    ASSERT_TRUE(encoded);

    tmcmt::v1::TrackletChunk chunk;
    ASSERT_TRUE(chunk.ParseFromString(encoded.value()));
    chunk.mutable_embedding()->set_dim(3);

    std::string payload;
    ASSERT_TRUE(chunk.SerializeToString(&payload));

    auto decoded = TrackletCodec::DecodeBinary(payload);
    EXPECT_FALSE(decoded);
    EXPECT_EQ(decoded.status().code(), tmcmt::ErrorCode::ParseError);
}

TEST(CalibrationCodec, RoundtripsBinaryAndJson) {
    tmcmt::CameraCalibration calibration;
    calibration.camera_id = "cam-01";
    calibration.has_homography = true;
    calibration.homography = {1.0, 0.0, 10.0, 0.0, 1.0, 20.0, 0.0, 0.0, 1.0};
    calibration.entry_zone.xy = {0.f, 0.f, 10.f, 0.f, 10.f, 10.f, 0.f, 10.f};
    calibration.meters_per_pixel_at_ground = 0.05f;
    calibration.frame_id = "map";

    auto encoded = CalibrationCodec::EncodeBinary(calibration);
    ASSERT_TRUE(encoded) << encoded.status().ToString();
    auto decoded = CalibrationCodec::DecodeBinary(encoded.value());
    ASSERT_TRUE(decoded) << decoded.status().ToString();
    EXPECT_EQ(decoded.value().camera_id, calibration.camera_id);
    EXPECT_TRUE(decoded.value().has_homography);
    EXPECT_EQ(decoded.value().homography, calibration.homography);
    EXPECT_EQ(decoded.value().entry_zone.xy, calibration.entry_zone.xy);

    auto json = CalibrationCodec::EncodeJson(calibration);
    ASSERT_TRUE(json) << json.status().ToString();
    auto fromJson = CalibrationCodec::DecodeJson(json.value());
    ASSERT_TRUE(fromJson) << fromJson.status().ToString();
    EXPECT_EQ(fromJson.value().camera_id, calibration.camera_id);
    EXPECT_FLOAT_EQ(fromJson.value().meters_per_pixel_at_ground, calibration.meters_per_pixel_at_ground);
}

} // namespace
