#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "trafficmcmt/core/GlobalTrack.h"
#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/io/IResultSink.h"
#include "trafficmcmt/io/TrackletCodec.h"
#include "trafficmcmt/net/MessageFramer.h"
#include "trafficmcmt/net/ZmqContext.h"

namespace tmcmt::net {

struct ZmqResultSinkOptions {
    std::string endpoint = "tcp://*:5558";
    bool bind = true;
    int snd_hwm = 10'000;
    int linger_ms = 0;
    TrackletCodecOptions codec;
    FramerOptions framer;
};

struct ZmqResultSinkStats {
    std::uint64_t published = 0;
    std::uint64_t merge_events = 0;
    std::uint64_t send_failures = 0;
    std::uint64_t bytes_sent = 0;
};

/// Master-side PUB socket for `GlobalTrackUpdate` and `GlobalIdMergeEvent`.
///
/// Topics are `tmcmt.v1.global.track` and `tmcmt.v1.global.events`; the payload
/// is the corresponding protobuf message. Sends are non-blocking.
class ZmqResultSink final : public IResultSink {
public:
    ZmqResultSink();
    ~ZmqResultSink() override;

    ZmqResultSink(const ZmqResultSink&) = delete;
    ZmqResultSink& operator=(const ZmqResultSink&) = delete;

    [[nodiscard]] Status Start(ZmqResultSinkOptions options);
    void Stop() noexcept;

    [[nodiscard]] Status Publish(const GlobalTrackUpdate& update) override;
    [[nodiscard]] Status PublishMerge(const GlobalIdMergeEvent& event) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] const ZmqResultSinkStats& stats() const noexcept {
        return m_stats;
    }

private:
    [[nodiscard]] Status Send(std::string_view topic, std::string_view payload);

    ZmqResultSinkOptions m_options;
    ZmqContext m_context;
    void* m_socket = nullptr;
    MessageFramer m_framer;
    ZmqResultSinkStats m_stats;
    std::string m_endpoint;
};

} // namespace tmcmt::net
