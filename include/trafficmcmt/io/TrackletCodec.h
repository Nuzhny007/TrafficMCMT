#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Camera.h"
#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"

namespace tmcmt {

/// Options that affect the binary encoding only.
struct TrackletCodecOptions {
    /// Encode the embedding as int8 (lossy, ~4x smaller). Decoding is always
    /// automatic: the wire format carries the encoding tag and the scale.
    bool quantize_embeddings = false;
};

/// Converts between the domain `Tracklet` and the protobuf wire format.
///
/// Three representations are supported:
///   * `Binary`  — raw `tmcmt.v1.TrackletChunk` protobuf message;
///   * `Framed`  — varint length prefix + `Binary` payload, suitable for
///                 concatenation in a single file or a ZeroMQ frame;
///   * `Json`    — protobuf JSON mapping, one object per tracklet, used for
///                 debugging and for human-readable golden files.
class TrackletCodec {
public:
    [[nodiscard]] static Result<std::string> EncodeBinary(const Tracklet& tracklet,
                                                          const TrackletCodecOptions& options = {});
    [[nodiscard]] static Result<Tracklet> DecodeBinary(std::string_view payload);

    [[nodiscard]] static Result<std::string> EncodeFramed(const Tracklet& tracklet,
                                                          const TrackletCodecOptions& options = {});
    /// Decodes exactly one framed message. When `consumed` is not null it
    /// receives the number of bytes read, including the length prefix.
    [[nodiscard]] static Result<Tracklet> DecodeFramed(std::string_view payload, std::size_t* consumed = nullptr);

    [[nodiscard]] static Result<std::string> EncodeJson(const Tracklet& tracklet, bool pretty = false);
    [[nodiscard]] static Result<Tracklet> DecodeJson(std::string_view json);
};

/// Same representations for camera calibration.
class CalibrationCodec {
public:
    [[nodiscard]] static Result<std::string> EncodeBinary(const CameraCalibration& calibration);
    [[nodiscard]] static Result<CameraCalibration> DecodeBinary(std::string_view payload);

    [[nodiscard]] static Result<std::string> EncodeFramed(const CameraCalibration& calibration);
    [[nodiscard]] static Result<CameraCalibration> DecodeFramed(std::string_view payload,
                                                               std::size_t* consumed = nullptr);

    [[nodiscard]] static Result<std::string> EncodeJson(const CameraCalibration& calibration, bool pretty = false);
    [[nodiscard]] static Result<CameraCalibration> DecodeJson(std::string_view json);
};

/// Master -> consumer messages: global track snapshots and identity merges.
///
/// `EncodeBinary` produces a raw `tmcmt.v1.GlobalTrackUpdate` (or
/// `GlobalIdMergeEvent`) payload, which is what the result sink puts into the
/// ZeroMQ frame. `EncodeJson` uses the protobuf JSON mapping.
class GlobalTrackCodec {
public:
    [[nodiscard]] static Result<std::string> EncodeBinary(const GlobalTrackUpdate& update,
                                                          const TrackletCodecOptions& options = {});
    [[nodiscard]] static Result<GlobalTrackUpdate> DecodeBinary(std::string_view payload);

    [[nodiscard]] static Result<std::string> EncodeJson(const GlobalTrackUpdate& update, bool pretty = false);
    [[nodiscard]] static Result<GlobalTrackUpdate> DecodeJson(std::string_view json);
};

class GlobalIdMergeCodec {
public:
    [[nodiscard]] static Result<std::string> EncodeBinary(const GlobalIdMergeEvent& event);
    [[nodiscard]] static Result<GlobalIdMergeEvent> DecodeBinary(std::string_view payload);

    [[nodiscard]] static Result<std::string> EncodeJson(const GlobalIdMergeEvent& event, bool pretty = false);
    [[nodiscard]] static Result<GlobalIdMergeEvent> DecodeJson(std::string_view json);
};

} // namespace tmcmt
