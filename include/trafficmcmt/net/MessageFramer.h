#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net {

/// Topic layout: `tmcmt.<major>.<camera_id>.<kind>`.
inline constexpr std::string_view kZmqTopicPrefix = "tmcmt.v1.";

enum class MessageKind : std::uint8_t {
    Tracklet = 0,
    Status,
    Calibration,
    GlobalTrack,
    GlobalEvents,
    Unknown,
};

struct Topic {
    std::string text;
    unsigned version = 1;
    /// Empty for global topics (`calib`, `global.*`).
    std::string camera_id;
    MessageKind kind = MessageKind::Unknown;
};

struct FramerOptions {
    std::string prefix = std::string(kZmqTopicPrefix);
    /// Hard limit for a single payload frame. Protects the receiver from a
    /// malformed or hostile length field.
    std::size_t max_payload_bytes = 8U << 20;
};

/// Builds and validates ZeroMQ multipart frames.
///
/// The message itself is always `[topic][payload]`; the framer owns only the
/// topic grammar and the payload size limit. Protobuf decoding is done by
/// `TrackletCodec` / `GlobalTrackCodec`.
class MessageFramer {
public:
    explicit MessageFramer(FramerOptions options = {});

    [[nodiscard]] const FramerOptions& options() const noexcept {
        return m_options;
    }

    /// Returns `tmcmt.v1.<camera>.<kind>` or a global topic. Camera ids must
    /// not contain dots or whitespace.
    [[nodiscard]] Result<std::string> MakeTopic(std::string_view camera_id, MessageKind kind) const;

    /// Parses a topic and rejects unknown kinds, empty camera ids and foreign
    /// protocol versions.
    [[nodiscard]] Result<Topic> ParseTopic(std::string_view topic) const;

    [[nodiscard]] Status ValidatePayload(std::string_view payload) const;

    [[nodiscard]] static std::string_view KindName(MessageKind kind) noexcept;
    [[nodiscard]] static MessageKind KindFromName(std::string_view name) noexcept;
    [[nodiscard]] static bool IsGlobal(MessageKind kind) noexcept;

private:
    FramerOptions m_options;
};

} // namespace tmcmt::net
