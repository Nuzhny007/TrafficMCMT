#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net {

/// Minimal RFC 6455 client used by integration tests and diagnostics.
class WebSocketClient {
public:
    [[nodiscard]] static Result<std::unique_ptr<WebSocketClient>> Connect(const std::string& host,
                                                                          std::uint16_t port,
                                                                          const std::string& path = "/ws",
                                                                          int timeout_ms = 3000);

    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    [[nodiscard]] Status SendText(std::string_view text);

    /// Reads the next text frame; ping frames are answered transparently.
    [[nodiscard]] Result<std::string> ReceiveText(int timeout_ms = 3000);

    void Close();

    [[nodiscard]] bool Connected() const noexcept;

private:
    struct Impl;
    explicit WebSocketClient(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace tmcmt::net
