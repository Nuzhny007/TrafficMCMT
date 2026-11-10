#include "trafficmcmt/net/MessageFramer.h"

#include <cctype>
#include <utility>

namespace tmcmt::net {
namespace {

bool HasWhitespace(std::string_view text) {
    for (const char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            return true;
        }
    }
    return false;
}

Status Invalid(std::string message) {
    return Status::Error(ErrorCode::InvalidArgument, std::move(message));
}

} // namespace

MessageFramer::MessageFramer(FramerOptions options) : m_options(std::move(options)) {
    if (m_options.prefix.empty()) {
        m_options.prefix = std::string(kZmqTopicPrefix);
    }
}

std::string_view MessageFramer::KindName(MessageKind kind) noexcept {
    switch (kind) {
    case MessageKind::Tracklet:
        return "tracklet";
    case MessageKind::Status:
        return "status";
    case MessageKind::Calibration:
        return "calib";
    case MessageKind::GlobalTrack:
        return "track";
    case MessageKind::GlobalEvents:
        return "events";
    case MessageKind::Unknown:
        break;
    }
    return "unknown";
}

MessageKind MessageFramer::KindFromName(std::string_view name) noexcept {
    if (name == "tracklet") {
        return MessageKind::Tracklet;
    }
    if (name == "status") {
        return MessageKind::Status;
    }
    if (name == "calib") {
        return MessageKind::Calibration;
    }
    if (name == "track") {
        return MessageKind::GlobalTrack;
    }
    if (name == "events") {
        return MessageKind::GlobalEvents;
    }
    return MessageKind::Unknown;
}

bool MessageFramer::IsGlobal(MessageKind kind) noexcept {
    return kind == MessageKind::Calibration || kind == MessageKind::GlobalTrack ||
           kind == MessageKind::GlobalEvents;
}

Result<std::string> MessageFramer::MakeTopic(std::string_view camera_id, MessageKind kind) const {
    if (kind == MessageKind::Unknown) {
        return Invalid("cannot build a topic for MessageKind::Unknown");
    }

    if (kind == MessageKind::Calibration) {
        return m_options.prefix + "calib";
    }
    if (kind == MessageKind::GlobalTrack) {
        return m_options.prefix + "global.track";
    }
    if (kind == MessageKind::GlobalEvents) {
        return m_options.prefix + "global.events";
    }

    if (camera_id.empty()) {
        return Invalid("camera_id must not be empty for a per-camera topic");
    }
    if (camera_id.find('.') != std::string_view::npos) {
        return Invalid("camera_id must not contain '.'");
    }
    if (HasWhitespace(camera_id)) {
        return Invalid("camera_id must not contain whitespace");
    }

    std::string topic;
    topic.reserve(m_options.prefix.size() + camera_id.size() + 1 + KindName(kind).size());
    topic.append(m_options.prefix);
    topic.append(camera_id);
    topic.push_back('.');
    topic.append(KindName(kind));
    return topic;
}

Result<Topic> MessageFramer::ParseTopic(std::string_view topic) const {
    if (!topic.starts_with(m_options.prefix)) {
        return Status::Error(ErrorCode::Unsupported,
                             "unexpected topic prefix in '" + std::string(topic) + "', expected '" +
                                 m_options.prefix + "'");
    }

    std::string_view rest = topic.substr(m_options.prefix.size());
    Topic out;
    out.text = std::string(topic);
    out.version = 1;

    if (rest == "calib") {
        out.kind = MessageKind::Calibration;
        return out;
    }
    if (rest.starts_with("global.")) {
        const std::string_view name = rest.substr(std::string_view("global.").size());
        out.kind = KindFromName(name);
        if (out.kind != MessageKind::GlobalTrack && out.kind != MessageKind::GlobalEvents) {
            return Invalid("unknown global topic '" + std::string(topic) + "'");
        }
        return out;
    }

    const std::size_t dot = rest.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size()) {
        return Invalid("malformed topic '" + std::string(topic) + "'");
    }

    out.camera_id = std::string(rest.substr(0, dot));
    if (out.camera_id.find('.') != std::string::npos) {
        return Invalid("camera_id must not contain '.' in topic '" + std::string(topic) + "'");
    }
    const std::string_view name = rest.substr(dot + 1);
    out.kind = KindFromName(name);
    if (out.kind == MessageKind::Unknown) {
        return Invalid("unknown message kind '" + std::string(name) + "' in topic '" + std::string(topic) + "'");
    }
    if (IsGlobal(out.kind)) {
        return Invalid("global kind '" + std::string(name) + "' cannot appear after a camera id");
    }
    return out;
}

Status MessageFramer::ValidatePayload(std::string_view payload) const {
    if (payload.empty()) {
        return Invalid("ZeroMQ payload is empty");
    }
    if (payload.size() > m_options.max_payload_bytes) {
        return Invalid("ZeroMQ payload is " + std::to_string(payload.size()) + " bytes, limit is " +
                       std::to_string(m_options.max_payload_bytes));
    }
    return Status::Ok();
}

} // namespace tmcmt::net
