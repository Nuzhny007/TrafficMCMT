#include "trafficmcmt/net/ZmqTrackletSource.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include <zmq.h>

#include "ZmqCommon.h"
#include "trafficmcmt/io/TrackletCodec.h"

namespace tmcmt::net {
namespace {

int RemainingMs(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<std::int64_t>(remaining, 2'147'483'647));
}

} // namespace

ZmqTrackletSource::ZmqTrackletSource() = default;

ZmqTrackletSource::~ZmqTrackletSource() {
    Stop();
    if (m_socket != nullptr) {
        zmq_close(m_socket);
        m_socket = nullptr;
    }
    m_context.Close();
}

Status ZmqTrackletSource::Start(ZmqSourceOptions options) {
    if (m_socket != nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ source is already started");
    }
    if (options.endpoint.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "source endpoint must not be empty");
    }

    m_options = std::move(options);
    m_options.framer.max_payload_bytes = m_options.max_payload_bytes;
    m_framer = MessageFramer(m_options.framer);
    m_normalizer = TrackletNormalizer(TrackletNormalizer::Options{
        m_options.expected_schema_version, true, false, m_options.max_seen_keys});
    m_sequences = SequenceTracker{};
    m_stats = ZmqSourceStats{};
    m_stop.store(false);

    Status status = m_context.Init(1);
    if (!status.ok()) {
        return status;
    }

    detail::Socket socket(zmq_socket(m_context.handle(), ZMQ_SUB));
    if (!socket) {
        status = detail::MakeZmqError("zmq_socket(ZMQ_SUB)");
        m_context.Close();
        return status;
    }

    if (!(status = detail::SetInt(socket.get(), ZMQ_RCVHWM, m_options.rcv_hwm)).ok() ||
        !(status = detail::SetInt(socket.get(), ZMQ_LINGER, 0)).ok() ||
        !(status = detail::SetInt(socket.get(), ZMQ_RECONNECT_IVL, m_options.reconnect_ivl_ms)).ok()) {
        m_context.Close();
        return status;
    }

    if (m_options.heartbeat) {
        detail::TrySetInt(socket.get(), ZMQ_HEARTBEAT_IVL, m_options.heartbeat_ivl_ms);
        detail::TrySetInt(socket.get(), ZMQ_HEARTBEAT_TIMEOUT, m_options.heartbeat_timeout_ms);
    }

    if (!(status = detail::SetString(socket.get(), ZMQ_SUBSCRIBE, m_options.subscribe_prefix)).ok()) {
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

void ZmqTrackletSource::Stop() noexcept {
    m_stop.store(true);
    // `zmq_ctx_shutdown` makes a blocking `zmq_poll`/`zmq_msg_recv` return
    // ETERM, which unblocks a `Next()` running on another thread.
    m_context.Shutdown();
}

Result<bool> ZmqTrackletSource::Next(Tracklet& out) {
    if (m_socket == nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ source is not started");
    }

    const bool infinite = m_options.timeout_ms < 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
                                                                  infinite ? 0 : m_options.timeout_ms);

    for (;;) {
        if (m_stop.load()) {
            return false;
        }

        const int timeout = infinite ? -1 : RemainingMs(deadline);
        zmq_pollitem_t item{};
        item.socket = m_socket;
        item.events = ZMQ_POLLIN;

        const int rc = zmq_poll(&item, 1, timeout);
        if (rc < 0) {
            const int err = zmq_errno();
            if (err == EINTR) {
                continue;
            }
            if (detail::IsTerminated(err) || m_stop.load()) {
                return false;
            }
            return detail::MakeZmqError("zmq_poll", err);
        }
        if (rc == 0) {
            if (m_stop.load()) {
                return false;
            }
            ++m_stats.timeouts;
            return Status::Error(ErrorCode::Timeout, "no ZeroMQ message received within timeout");
        }

        auto frames = detail::ReceiveFrames(m_socket, 0);
        if (!frames) {
            if (m_stop.load()) {
                return false;
            }
            ++m_stats.malformed;
            if (m_options.strict) {
                return frames.status();
            }
            continue;
        }

        const std::vector<std::string>& parts = frames.value();
        if (parts.size() != 2) {
            ++m_stats.malformed;
            if (m_options.strict) {
                return Status::Error(ErrorCode::ParseError,
                                     "expected a 2-frame ZeroMQ message [topic][payload], got " +
                                         std::to_string(parts.size()));
            }
            continue;
        }

        const std::string& topic = parts[0];
        const std::string& payload = parts[1];
        ++m_stats.received;
        m_stats.bytes_received += topic.size() + payload.size();

        auto parsed = m_framer.ParseTopic(topic);
        if (!parsed) {
            ++m_stats.malformed;
            if (m_options.strict) {
                return parsed.status();
            }
            continue;
        }
        if (parsed.value().kind != MessageKind::Tracklet) {
            ++m_stats.skipped_topics;
            continue;
        }

        Status status = m_framer.ValidatePayload(payload);
        if (!status.ok()) {
            ++m_stats.malformed;
            if (m_options.strict) {
                return status;
            }
            continue;
        }

        auto decoded = TrackletCodec::DecodeBinary(payload);
        if (!decoded) {
            ++m_stats.rejected;
            if (m_options.strict) {
                return decoded.status();
            }
            continue;
        }

        Tracklet tracklet = std::move(decoded).value();
        if (m_options.normalize) {
            auto normalized = m_normalizer.Normalize(std::move(tracklet));
            if (!normalized) {
                ++m_stats.rejected;
                if (m_options.strict) {
                    return normalized.status();
                }
                continue;
            }
            if (normalized.value().status == NormalizeStatus::Duplicate) {
                ++m_stats.duplicates;
                continue;
            }
            tracklet = std::move(normalized.value().tracklet);
        }

        const SequenceEvent event = m_sequences.Observe(tracklet.session_id, tracklet.camera, tracklet.sequence);
        switch (event.kind) {
        case SequenceEventKind::Gap:
            m_stats.missing_total += event.missing;
            break;
        case SequenceEventKind::Duplicate:
            ++m_stats.duplicates;
            break;
        case SequenceEventKind::OutOfOrder:
            ++m_stats.out_of_order;
            break;
        case SequenceEventKind::First:
        case SequenceEventKind::InOrder:
            break;
        }

        ++m_stats.decoded;
        out = std::move(tracklet);
        return true;
    }
}

Status ZmqTrackletSource::Reset() {
    m_normalizer.Reset();
    m_sequences.Reset();
    return Status::Ok();
}

bool ZmqTrackletSource::running() const noexcept {
    return m_socket != nullptr && !m_stop.load();
}

std::string ZmqTrackletSource::endpoint() const {
    return m_endpoint;
}

std::string ZmqTrackletSource::Describe() const {
    if (!running()) {
        return "zmq-source(stopped)";
    }
    return "zmq-source(" + m_endpoint + ", decoded=" + std::to_string(m_stats.decoded) +
           ", missing=" + std::to_string(m_stats.missing_total) + ")";
}

} // namespace tmcmt::net
