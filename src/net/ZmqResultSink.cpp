#include "trafficmcmt/net/ZmqResultSink.h"

#include <utility>
#include <vector>

#include <zmq.h>

#include "ZmqCommon.h"

namespace tmcmt::net {

ZmqResultSink::ZmqResultSink() = default;

ZmqResultSink::~ZmqResultSink() {
    Stop();
}

Status ZmqResultSink::Start(ZmqResultSinkOptions options) {
    if (m_socket != nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ result sink is already started");
    }
    if (options.endpoint.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "result sink endpoint must not be empty");
    }

    m_options = std::move(options);
    m_framer = MessageFramer(m_options.framer);
    m_stats = ZmqResultSinkStats{};

    Status status = m_context.Init(1);
    if (!status.ok()) {
        return status;
    }

    detail::Socket socket(zmq_socket(m_context.handle(), ZMQ_PUB));
    if (!socket) {
        status = detail::MakeZmqError("zmq_socket(ZMQ_PUB)");
        m_context.Close();
        return status;
    }

    if (!(status = detail::SetInt(socket.get(), ZMQ_SNDHWM, m_options.snd_hwm)).ok() ||
        !(status = detail::SetInt(socket.get(), ZMQ_LINGER, m_options.linger_ms)).ok()) {
        m_context.Close();
        return status;
    }

    if (m_options.bind) {
        status = detail::Bind(socket.get(), m_options.endpoint);
    } else {
        status = detail::Connect(socket.get(), m_options.endpoint);
    }
    if (!status.ok()) {
        m_context.Close();
        return status;
    }

    auto endpoint = detail::LastEndpoint(socket.get());
    if (!endpoint) {
        m_context.Close();
        return endpoint.status();
    }

    m_endpoint = std::move(endpoint).value();
    m_socket = socket.Release();
    return Status::Ok();
}

void ZmqResultSink::Stop() noexcept {
    if (m_socket != nullptr) {
        zmq_close(m_socket);
        m_socket = nullptr;
    }
    m_context.Close();
    m_endpoint.clear();
}

Status ZmqResultSink::Publish(const GlobalTrackUpdate& update) {
    if (m_socket == nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ result sink is not started");
    }

    auto topic = m_framer.MakeTopic({}, MessageKind::GlobalTrack);
    if (!topic) {
        return topic.status();
    }
    auto payload = GlobalTrackCodec::EncodeBinary(update, m_options.codec);
    if (!payload) {
        return payload.status();
    }

    Status status = m_framer.ValidatePayload(payload.value());
    if (!status.ok()) {
        return status;
    }
    status = Send(topic.value(), payload.value());
    if (status.ok()) {
        ++m_stats.published;
    }
    return status;
}

Status ZmqResultSink::PublishMerge(const GlobalIdMergeEvent& event) {
    if (m_socket == nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ result sink is not started");
    }

    auto topic = m_framer.MakeTopic({}, MessageKind::GlobalEvents);
    if (!topic) {
        return topic.status();
    }
    auto payload = GlobalIdMergeCodec::EncodeBinary(event);
    if (!payload) {
        return payload.status();
    }

    Status status = m_framer.ValidatePayload(payload.value());
    if (!status.ok()) {
        return status;
    }
    status = Send(topic.value(), payload.value());
    if (status.ok()) {
        ++m_stats.merge_events;
    }
    return status;
}

Status ZmqResultSink::Flush() {
    return Status::Ok();
}

Status ZmqResultSink::Send(std::string_view topic, std::string_view payload) {
    const std::vector<std::string_view> frames{topic, payload};
    const int rc = zmq_send(m_socket, frames[0].data(), frames[0].size(), ZMQ_SNDMORE | ZMQ_DONTWAIT);
    if (rc < 0) {
        const int err = zmq_errno();
        ++m_stats.send_failures;
        if (detail::IsEagain(err)) {
            return Status::Error(ErrorCode::Timeout, "result sink send queue is full");
        }
        return detail::MakeZmqError("zmq_send(topic)", err);
    }
    if (zmq_send(m_socket, frames[1].data(), frames[1].size(), ZMQ_DONTWAIT) < 0) {
        const int err = zmq_errno();
        ++m_stats.send_failures;
        if (detail::IsEagain(err)) {
            return Status::Error(ErrorCode::Timeout, "result sink send queue is full");
        }
        return detail::MakeZmqError("zmq_send(payload)", err);
    }

    m_stats.bytes_sent += topic.size() + payload.size();
    return Status::Ok();
}

bool ZmqResultSink::running() const noexcept {
    return m_socket != nullptr;
}

std::string ZmqResultSink::endpoint() const {
    return m_endpoint;
}

std::string ZmqResultSink::Describe() const {
    if (!running()) {
        return "zmq-result-sink(stopped)";
    }
    return "zmq-result-sink(" + m_endpoint + ", published=" + std::to_string(m_stats.published) +
           ", merges=" + std::to_string(m_stats.merge_events) + ")";
}

} // namespace tmcmt::net
