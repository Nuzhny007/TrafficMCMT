#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/TrackletCodec.h"
#include "trafficmcmt/net/MessageFramer.h"
#include "trafficmcmt/net/ZmqContext.h"

namespace tmcmt::net {

struct ZmqPublisherOptions {
    /// `tcp://*:5556` (bind) or `tcp://host:5556` (connect).
    std::string endpoint = "tcp://*:5556";
    bool bind = true;
    int snd_hwm = 10'000;
    int linger_ms = 0;
    /// Used when `Tracklet::camera` is empty.
    std::string camera_id;
    TrackletCodecOptions codec;
    FramerOptions framer;
};

struct ZmqPublisherStats {
    std::uint64_t published = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t bytes_sent = 0;
};

/// Edge-side PUB socket that publishes `TrackletChunk` messages.
///
/// One publisher is usually created per camera; the topic is derived from
/// `Tracklet::camera`. Sends are non-blocking: when the outgoing queue is full
/// the message is dropped and counted in `send_failures` instead of stalling
/// the tracker.
class ZmqTrackletPublisher {
public:
    ZmqTrackletPublisher();
    ~ZmqTrackletPublisher();

    ZmqTrackletPublisher(const ZmqTrackletPublisher&) = delete;
    ZmqTrackletPublisher& operator=(const ZmqTrackletPublisher&) = delete;

    [[nodiscard]] Status Start(ZmqPublisherOptions options);
    void Stop() noexcept;

    [[nodiscard]] Status Publish(const Tracklet& tracklet);

    /// Publishes an already encoded payload under an explicit topic.
    [[nodiscard]] Status PublishRaw(std::string_view topic, std::string_view payload);

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] const ZmqPublisherStats& stats() const noexcept {
        return m_stats;
    }
    [[nodiscard]] std::string Describe() const;

private:
    [[nodiscard]] Status Send(std::string_view topic, std::string_view payload);

    ZmqPublisherOptions m_options;
    ZmqContext m_context;
    void* m_socket = nullptr;
    MessageFramer m_framer;
    ZmqPublisherStats m_stats;
    std::string m_endpoint;
};

} // namespace tmcmt::net
