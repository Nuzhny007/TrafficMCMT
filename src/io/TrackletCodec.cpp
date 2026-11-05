#include "trafficmcmt/io/TrackletCodec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/json_util.h>

#include "calibration.pb.h"
#include "tracklet.pb.h"
#include "trafficmcmt/core/TrackletId.h"
#include "trafficmcmt/core/Version.h"

namespace tmcmt {
namespace {

namespace proto = ::tmcmt::v1;

Status MakeStatus(ErrorCode code, const std::string& message) {
    return Status::Error(code, message);
}

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

proto::ObjectType ToProto(ObjectType type) {
    switch (type) {
    case ObjectType::Unknown:
        return proto::OBJECT_TYPE_UNKNOWN;
    case ObjectType::Pedestrian:
        return proto::OBJECT_TYPE_PEDESTRIAN;
    case ObjectType::Car:
        return proto::OBJECT_TYPE_CAR;
    case ObjectType::Truck:
        return proto::OBJECT_TYPE_TRUCK;
    case ObjectType::Bus:
        return proto::OBJECT_TYPE_BUS;
    case ObjectType::Bike:
        return proto::OBJECT_TYPE_BIKE;
    }
    return proto::OBJECT_TYPE_UNKNOWN;
}

ObjectType FromProto(proto::ObjectType type) {
    switch (type) {
    case proto::OBJECT_TYPE_PEDESTRIAN:
        return ObjectType::Pedestrian;
    case proto::OBJECT_TYPE_CAR:
        return ObjectType::Car;
    case proto::OBJECT_TYPE_TRUCK:
        return ObjectType::Truck;
    case proto::OBJECT_TYPE_BUS:
        return ObjectType::Bus;
    case proto::OBJECT_TYPE_BIKE:
        return ObjectType::Bike;
    case proto::OBJECT_TYPE_UNKNOWN:
    default:
        return ObjectType::Unknown;
    }
}

// ---------------------------------------------------------------------------
// Domain -> proto
// ---------------------------------------------------------------------------

void ToProto(const Embedding& embedding, proto::Embedding& out, bool quantize) {
    out.set_dim(embedding.dim);
    out.set_model_id(embedding.model_id);

    if (!quantize) {
        out.set_encoding(proto::Embedding::ENCODING_FLOAT32);
        out.mutable_values()->Reserve(static_cast<int>(embedding.values.size()));
        for (const float value : embedding.values) {
            out.add_values(value);
        }
        return;
    }

    float maxAbs = 0.f;
    for (const float value : embedding.values) {
        maxAbs = std::max(maxAbs, std::fabs(value));
    }
    const float scale = maxAbs > 0.f ? maxAbs / 127.f : 0.f;

    std::string quantized(embedding.values.size(), '\0');
    for (std::size_t i = 0; i < embedding.values.size(); ++i) {
        long q = 0;
        if (scale > 0.f) {
            q = std::lround(static_cast<double>(embedding.values[i]) / static_cast<double>(scale));
            q = std::clamp(q, -127L, 127L);
        }
        quantized[i] = static_cast<char>(static_cast<signed char>(q));
    }

    out.set_encoding(proto::Embedding::ENCODING_INT8);
    out.set_scale(scale);
    out.set_quantized(std::move(quantized));
}

void ToProto(const Observation& observation, proto::Observation& out) {
    out.set_timestamp_ms(static_cast<std::int64_t>(observation.ts.count()));

    auto* bbox = out.mutable_bbox();
    bbox->set_x(observation.bbox.x);
    bbox->set_y(observation.bbox.y);
    bbox->set_w(observation.bbox.w);
    bbox->set_h(observation.bbox.h);
    bbox->set_score(observation.bbox.score);

    auto* world = out.mutable_world();
    world->set_x(observation.world.x);
    world->set_y(observation.world.y);
    world->set_valid(observation.world.valid);
}

void ToProto(const Tracklet& tracklet, proto::TrackletChunk& out, const TrackletCodecOptions& options) {
    out.Clear();
    out.set_schema_version(tracklet.schema_version);
    out.set_session_id(tracklet.session_id);
    out.set_camera_id(tracklet.camera);
    out.set_local_track_id(tracklet.local_track_id);
    out.set_sequence(tracklet.sequence);
    out.set_begin_ms(static_cast<std::int64_t>(tracklet.begin.count()));
    out.set_end_ms(static_cast<std::int64_t>(tracklet.end.count()));

    out.mutable_observations()->Reserve(static_cast<int>(tracklet.observations.size()));
    for (const Observation& observation : tracklet.observations) {
        ToProto(observation, *out.add_observations());
    }

    ToProto(tracklet.embedding, *out.mutable_embedding(), options.quantize_embeddings);
    out.set_type(ToProto(tracklet.type));
    out.set_quality(tracklet.quality);
    out.set_kind(tracklet.is_end ? proto::MESSAGE_KIND_TRACK_END : proto::MESSAGE_KIND_TRACKLET);
    out.set_origin_lat(tracklet.origin_latitude);
    out.set_origin_lon(tracklet.origin_longitude);
}

void ToProto(const CameraCalibration& calibration, proto::CameraCalibration& out) {
    out.Clear();
    out.set_schema_version(kProtocolVersion);
    out.set_camera_id(calibration.camera_id);
    out.set_has_homography(calibration.has_homography);
    out.mutable_homography()->Reserve(static_cast<int>(calibration.homography.size()));
    for (const double value : calibration.homography) {
        out.add_homography(value);
    }

    auto* entry = out.mutable_entry_zone();
    entry->mutable_xy()->Reserve(static_cast<int>(calibration.entry_zone.xy.size()));
    for (const float value : calibration.entry_zone.xy) {
        entry->add_xy(value);
    }

    auto* exit = out.mutable_exit_zone();
    exit->mutable_xy()->Reserve(static_cast<int>(calibration.exit_zone.xy.size()));
    for (const float value : calibration.exit_zone.xy) {
        exit->add_xy(value);
    }

    out.set_meters_per_pixel_at_ground(calibration.meters_per_pixel_at_ground);
    out.set_frame_id(calibration.frame_id);
    out.set_updated_at_ms(calibration.updated_at_ms);
}

// ---------------------------------------------------------------------------
// Proto -> domain
// ---------------------------------------------------------------------------

Result<Embedding> FromProto(const proto::Embedding& embedding) {
    if (embedding.dim() == 0) {
        return MakeStatus(ErrorCode::InvalidArgument, "embedding.dim must be > 0");
    }
    if (embedding.dim() > std::numeric_limits<std::uint16_t>::max()) {
        return MakeStatus(ErrorCode::InvalidArgument, "embedding.dim exceeds 65535");
    }

    Embedding out;
    out.dim = static_cast<std::uint16_t>(embedding.dim());
    out.model_id = embedding.model_id();
    out.values.resize(embedding.dim());

    switch (embedding.encoding()) {
    case proto::Embedding::ENCODING_FLOAT32: {
        if (embedding.values_size() != static_cast<int>(embedding.dim())) {
            return MakeStatus(ErrorCode::ParseError,
                              "embedding.values has " + std::to_string(embedding.values_size()) +
                                  " entries, expected " + std::to_string(embedding.dim()));
        }
        for (int i = 0; i < embedding.values_size(); ++i) {
            const float value = embedding.values(i);
            if (!std::isfinite(value)) {
                return MakeStatus(ErrorCode::InvalidArgument, "embedding contains a non-finite value");
            }
            out.values[static_cast<std::size_t>(i)] = value;
        }
        break;
    }
    case proto::Embedding::ENCODING_INT8: {
        const std::string& payload = embedding.quantized();
        if (payload.size() != static_cast<std::size_t>(embedding.dim())) {
            return MakeStatus(ErrorCode::ParseError,
                              "embedding.quantized has " + std::to_string(payload.size()) +
                                  " bytes, expected " + std::to_string(embedding.dim()));
        }
        const float scale = embedding.scale();
        if (!std::isfinite(scale)) {
            return MakeStatus(ErrorCode::InvalidArgument, "embedding.scale is not finite");
        }
        for (std::size_t i = 0; i < payload.size(); ++i) {
            const auto q = static_cast<signed char>(static_cast<unsigned char>(payload[i]));
            out.values[i] = static_cast<float>(q) * scale;
        }
        break;
    }
    default:
        return MakeStatus(ErrorCode::Unsupported,
                          "unknown embedding encoding " + std::to_string(static_cast<int>(embedding.encoding())));
    }

    return out;
}

Result<Observation> FromProto(const proto::Observation& observation) {
    Observation out;
    out.ts = Timestamp(observation.timestamp_ms());

    const proto::BBox& bbox = observation.bbox();
    out.bbox.x = bbox.x();
    out.bbox.y = bbox.y();
    out.bbox.w = bbox.w();
    out.bbox.h = bbox.h();
    out.bbox.score = bbox.score();
    if (!out.bbox.IsFinite()) {
        return MakeStatus(ErrorCode::InvalidArgument, "observation.bbox contains a non-finite value");
    }

    out.world.x = observation.world().x();
    out.world.y = observation.world().y();
    out.world.valid = observation.world().valid();
    if (out.world.valid && !out.world.IsFinite()) {
        return MakeStatus(ErrorCode::InvalidArgument, "observation.world contains a non-finite value");
    }
    return out;
}

Result<Tracklet> FromProto(const proto::TrackletChunk& chunk) {
    if (chunk.schema_version() != kProtocolVersion) {
        return MakeStatus(ErrorCode::Unsupported,
                          "unsupported schema_version " + std::to_string(chunk.schema_version()) + ", expected " +
                              std::to_string(kProtocolVersion));
    }
    if (chunk.session_id().empty()) {
        return MakeStatus(ErrorCode::InvalidArgument, "session_id must not be empty");
    }
    if (chunk.camera_id().empty()) {
        return MakeStatus(ErrorCode::InvalidArgument, "camera_id must not be empty");
    }
    if (chunk.begin_ms() > chunk.end_ms()) {
        return MakeStatus(ErrorCode::InvalidArgument, "begin_ms must be <= end_ms");
    }
    if (chunk.observations_size() == 0) {
        return MakeStatus(ErrorCode::InvalidArgument, "observations must not be empty");
    }

    Tracklet out;
    out.schema_version = chunk.schema_version();
    out.session_id = chunk.session_id();
    out.camera = chunk.camera_id();
    out.local_track_id = chunk.local_track_id();
    out.sequence = chunk.sequence();
    out.begin = Timestamp(chunk.begin_ms());
    out.end = Timestamp(chunk.end_ms());
    out.type = FromProto(chunk.type());
    out.quality = chunk.quality();
    out.is_end = chunk.kind() == proto::MESSAGE_KIND_TRACK_END;
    out.origin_latitude = chunk.origin_lat();
    out.origin_longitude = chunk.origin_lon();
    if (!std::isfinite(out.origin_latitude) || !std::isfinite(out.origin_longitude)) {
        return MakeStatus(ErrorCode::InvalidArgument, "tracklet origin is not finite");
    }

    out.observations.reserve(static_cast<std::size_t>(chunk.observations_size()));
    for (const proto::Observation& observation : chunk.observations()) {
        auto converted = FromProto(observation);
        if (!converted) {
            return converted.status();
        }
        out.observations.push_back(std::move(converted).value());
    }

    auto embedding = FromProto(chunk.embedding());
    if (!embedding) {
        return embedding.status();
    }
    out.embedding = std::move(embedding).value();

    AssignTrackletId(out);
    return out;
}

Result<CameraCalibration> FromProto(const proto::CameraCalibration& message) {
    if (message.schema_version() != kProtocolVersion) {
        return MakeStatus(ErrorCode::Unsupported,
                          "unsupported calibration schema_version " + std::to_string(message.schema_version()));
    }
    if (message.camera_id().empty()) {
        return MakeStatus(ErrorCode::InvalidArgument, "calibration.camera_id must not be empty");
    }

    CameraCalibration out;
    out.camera_id = message.camera_id();
    out.has_homography = message.has_homography();
    if (out.has_homography && message.homography_size() != 9) {
        return MakeStatus(ErrorCode::ParseError, "calibration.homography must contain exactly 9 values");
    }
    for (int i = 0; i < message.homography_size(); ++i) {
        out.homography[static_cast<std::size_t>(i)] = message.homography(i);
    }
    if (!out.IsValid()) {
        return MakeStatus(ErrorCode::InvalidArgument, "calibration is invalid");
    }

    out.entry_zone.xy.assign(message.entry_zone().xy().begin(), message.entry_zone().xy().end());
    out.exit_zone.xy.assign(message.exit_zone().xy().begin(), message.exit_zone().xy().end());
    out.meters_per_pixel_at_ground = message.meters_per_pixel_at_ground();
    out.frame_id = message.frame_id();
    out.updated_at_ms = message.updated_at_ms();
    return out;
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

void AppendVarint(std::string& out, std::uint64_t value) {
    while (value >= 0x80U) {
        out.push_back(static_cast<char>((value & 0x7FU) | 0x80U));
        value >>= 7U;
    }
    out.push_back(static_cast<char>(value));
}

Result<std::uint64_t> ReadVarint(std::string_view data, std::size_t& offset) {
    std::uint64_t value = 0;
    int shift = 0;
    while (offset < data.size() && shift <= 63) {
        const auto byte = static_cast<unsigned char>(data[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    return MakeStatus(ErrorCode::ParseError, "truncated or malformed varint length prefix");
}

Result<std::string> FramePayload(std::string payload) {
    std::string framed;
    framed.reserve(payload.size() + 10);
    AppendVarint(framed, static_cast<std::uint64_t>(payload.size()));
    framed.append(payload);
    return framed;
}

Result<std::string_view> UnframePayload(std::string_view data, std::size_t* consumed) {
    std::size_t offset = 0;
    auto length = ReadVarint(data, offset);
    if (!length) {
        return length.status();
    }
    const std::uint64_t size = length.value();
    if (size > data.size() - offset) {
        return MakeStatus(ErrorCode::ParseError, "framed message is truncated");
    }
    if (consumed != nullptr) {
        *consumed = offset + static_cast<std::size_t>(size);
    }
    return data.substr(offset, static_cast<std::size_t>(size));
}

template <typename Message>
Result<std::string> MessageToJson(const Message& message, bool pretty) {
    google::protobuf::util::JsonPrintOptions options;
    options.add_whitespace = pretty;
    options.always_print_fields_with_no_presence = true;
    options.preserve_proto_field_names = true;

    std::string json;
    const auto status = google::protobuf::util::MessageToJsonString(message, &json, options);
    if (!status.ok()) {
        return MakeStatus(ErrorCode::Internal, "protobuf JSON serialization failed: " + status.ToString());
    }
    return json;
}

template <typename Message>
Result<Message> JsonToMessage(std::string_view json) {
    google::protobuf::util::JsonParseOptions options;
    options.ignore_unknown_fields = false;

    Message message;
    const auto status = google::protobuf::util::JsonStringToMessage(std::string(json), &message, options);
    if (!status.ok()) {
        return MakeStatus(ErrorCode::ParseError, "invalid tracklet JSON: " + status.ToString());
    }
    return message;
}

} // namespace

// ---------------------------------------------------------------------------
// TrackletCodec
// ---------------------------------------------------------------------------

Result<std::string> TrackletCodec::EncodeBinary(const Tracklet& tracklet, const TrackletCodecOptions& options) {
    proto::TrackletChunk chunk;
    ToProto(tracklet, chunk, options);

    std::string payload;
    if (!chunk.SerializeToString(&payload)) {
        return MakeStatus(ErrorCode::Internal, "protobuf serialization failed");
    }
    return payload;
}

Result<Tracklet> TrackletCodec::DecodeBinary(std::string_view payload) {
    proto::TrackletChunk chunk;
    if (!chunk.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return MakeStatus(ErrorCode::ParseError, "cannot parse TrackletChunk protobuf message");
    }
    return FromProto(chunk);
}

Result<std::string> TrackletCodec::EncodeFramed(const Tracklet& tracklet, const TrackletCodecOptions& options) {
    auto payload = EncodeBinary(tracklet, options);
    if (!payload) {
        return payload.status();
    }
    return FramePayload(std::move(payload).value());
}

Result<Tracklet> TrackletCodec::DecodeFramed(std::string_view payload, std::size_t* consumed) {
    auto body = UnframePayload(payload, consumed);
    if (!body) {
        return body.status();
    }
    return DecodeBinary(body.value());
}

Result<std::string> TrackletCodec::EncodeJson(const Tracklet& tracklet, bool pretty) {
    proto::TrackletChunk chunk;
    ToProto(tracklet, chunk, TrackletCodecOptions{});
    return MessageToJson(chunk, pretty);
}

Result<Tracklet> TrackletCodec::DecodeJson(std::string_view json) {
    auto chunk = JsonToMessage<proto::TrackletChunk>(json);
    if (!chunk) {
        return chunk.status();
    }
    return FromProto(chunk.value());
}

// ---------------------------------------------------------------------------
// CalibrationCodec
// ---------------------------------------------------------------------------

Result<std::string> CalibrationCodec::EncodeBinary(const CameraCalibration& calibration) {
    proto::CameraCalibration message;
    ToProto(calibration, message);

    std::string payload;
    if (!message.SerializeToString(&payload)) {
        return MakeStatus(ErrorCode::Internal, "protobuf serialization failed");
    }
    return payload;
}

Result<CameraCalibration> CalibrationCodec::DecodeBinary(std::string_view payload) {
    proto::CameraCalibration message;
    if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return MakeStatus(ErrorCode::ParseError, "cannot parse CameraCalibration protobuf message");
    }
    return FromProto(message);
}

Result<std::string> CalibrationCodec::EncodeFramed(const CameraCalibration& calibration) {
    auto payload = EncodeBinary(calibration);
    if (!payload) {
        return payload.status();
    }
    return FramePayload(std::move(payload).value());
}

Result<CameraCalibration> CalibrationCodec::DecodeFramed(std::string_view payload, std::size_t* consumed) {
    auto body = UnframePayload(payload, consumed);
    if (!body) {
        return body.status();
    }
    return DecodeBinary(body.value());
}

Result<std::string> CalibrationCodec::EncodeJson(const CameraCalibration& calibration, bool pretty) {
    proto::CameraCalibration message;
    ToProto(calibration, message);
    return MessageToJson(message, pretty);
}

Result<CameraCalibration> CalibrationCodec::DecodeJson(std::string_view json) {
    auto message = JsonToMessage<proto::CameraCalibration>(json);
    if (!message) {
        return message.status();
    }
    return FromProto(message.value());
}

} // namespace tmcmt
