#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Result.h"
#include "trafficmcmt/core/Tracklet.h"
#include "trafficmcmt/io/ITrackletSource.h"
#include "trafficmcmt/io/TrackletNormalizer.h"
#include "trafficmcmt/net/MessageFramer.h"
#include "trafficmcmt/net/SequenceTracker.h"
#include "trafficmcmt/net/ZmqContext.h"

namespace tmcmt::net {

struct ZmqSourceOptions {
    /// `tcp://0.0.0.0:5556` (bind) or `tcp://host:5556` (connect).
    std::string endpoint = "tcp://0.0.0.0:5556";
    bool bind = false;
    std::string subscribe_prefix = std::string(kZmqTopicPrefix);
    int rcv_hwm = 5'000;
    int reconnect_ivl_ms = 1'000;
    /// `Next()` timeout. Negative blocks until a message or `Stop()`.
    int timeout_ms = 1'000;
    std::size_t max_payload_bytes = 8U << 20;
    /// Validate and canonicalise tracklets with `TrackletNormalizer`.
    bool normalize = true;
    /// Reject malformed messages with an error instead of skipping them.
    bool strict = true;
    std::uint32_t expected_schema_version = 1;
    /// Bounded duplicate-detection cache. Older keys are dropped when full so
    /// that long-running Master processes have a stable memory footprint.
    std::size_t max_seen_keys = 10'000;
    /// Best-effort liveness detection; ignored by old libzmq builds.
    bool heartbeat = true;
    int heartbeat_ivl_ms = 1'000;
    int heartbeat_timeout_ms = 3'000;
    FramerOptions framer;
};

struct ZmqSourceStats {
    std::uint64_t received = 0;
    std::uint64_t decoded = 0;
    std::uint64_t rejected = 0;
    std::uint64_t malformed = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t missing_total = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t skipped_topics = 0;
    std::uint64_t bytes_received = 0;
};

/// Master-side SUB socket that turns `TrackletChunk` frames into `Tracklet`s.
///
/// The source is a normal `ITrackletSource`: `Next()` returns `false` only when
/// the source is stopped, `Status::Error(ErrorCode::Timeout, ...)` when no
/// message arrived within `timeout_ms`, and a decoding error in strict mode.
/// Non-tracklet topics (status, calibration, global results) are skipped.
class ZmqTrackletSource final : public ITrackletSource {
public:
    ZmqTrackletSource();
    ~ZmqTrackletSource() override;

    ZmqTrackletSource(const ZmqTrackletSource&) = delete;
    ZmqTrackletSource& operator=(const ZmqTrackletSource&) = delete;

    [[nodiscard]] Status Start(ZmqSourceOptions options);

    /// Unblocks a pending `Next()` in another thread.
    void Stop() noexcept override;

    [[nodiscard]] Result<bool> Next(Tracklet& out) override;
    /// Reports the wall-clock time captured right after the message was
    /// received, before decoding. Used by the clock-synchronisation stage.
    [[nodiscard]] Result<bool> Next(Tracklet& out, Timestamp& received_at) override;
    [[nodiscard]] Status Reset() override;
    [[nodiscard]] std::string Describe() const override;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] const ZmqSourceStats& stats() const noexcept {
        return m_stats;
    }
    [[nodiscard]] const SequenceTracker& sequences() const noexcept {
        return m_sequences;
    }

private:
    ZmqSourceOptions m_options;
    ZmqContext m_context;
    void* m_socket = nullptr;
    std::atomic<bool> m_stop{false};
    MessageFramer m_framer;
    TrackletNormalizer m_normalizer;
    SequenceTracker m_sequences;
    ZmqSourceStats m_stats;
    std::string m_endpoint;
};

} // namespace tmcmt::net
