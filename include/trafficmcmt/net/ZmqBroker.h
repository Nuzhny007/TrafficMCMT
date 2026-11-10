#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net {

struct ZmqBrokerOptions {
    /// Publishers connect here (XSUB).
    std::string frontend_endpoint = "tcp://*:5555";
    /// Subscribers connect here (XPUB).
    std::string backend_endpoint = "tcp://*:5556";
    int hwm = 10'000;
    int linger_ms = 0;
};

struct ZmqBrokerStats {
    std::uint64_t forwarded_to_subscribers = 0;
    std::uint64_t forwarded_to_publishers = 0;
    std::uint64_t bytes_forwarded = 0;
    std::uint64_t errors = 0;
};

/// Minimal XSUB/XPUB forwarder.
///
/// It lets Edge publishers and Master subscribers connect to stable endpoints
/// and keeps running while either side reconnects. The broker is optional: a
/// single source can also bind its SUB socket directly.
class ZmqBroker {
public:
    ZmqBroker();
    ~ZmqBroker();

    ZmqBroker(const ZmqBroker&) = delete;
    ZmqBroker& operator=(const ZmqBroker&) = delete;

    [[nodiscard]] Status Start(ZmqBrokerOptions options);
    void Stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::string frontend_endpoint() const;
    [[nodiscard]] std::string backend_endpoint() const;
    [[nodiscard]] const ZmqBrokerStats& stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace tmcmt::net
