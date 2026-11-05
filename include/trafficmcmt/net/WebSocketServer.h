#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net {

struct WebSocketServerOptions {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    std::string path = "/ws";
    std::size_t max_clients = 32;
    std::size_t max_frame_bytes = 1U << 20;
    /// Outgoing queue limit per client; older frames are dropped when full.
    std::size_t max_pending_frames = 64;
};

/// Minimal RFC 6455 WebSocket server (text frames, ping/pong, close).
///
/// Thread model: one accept thread plus one thread per client. `Broadcast` and
/// `SendTo` only enqueue frames; sockets are written by the owning client
/// thread, so a slow client can never block the producer.
class WebSocketServer {
public:
    using ClientHandler = std::function<void(std::uint64_t)>;
    using TextHandler = std::function<void(std::uint64_t, std::string_view)>;
    using HealthProvider = std::function<std::string()>;

    explicit WebSocketServer(WebSocketServerOptions options);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    [[nodiscard]] Status Start();
    void Stop();

    [[nodiscard]] bool Running() const noexcept;
    [[nodiscard]] std::uint16_t Port() const noexcept;
    [[nodiscard]] std::size_t ClientCount() const noexcept;

    void Broadcast(std::string_view text);
    bool SendTo(std::uint64_t client_id, std::string_view text);

    void SetClientConnectedHandler(ClientHandler handler);
    void SetClientDisconnectedHandler(ClientHandler handler);
    void SetTextHandler(TextHandler handler);
    void SetHealthProvider(HealthProvider provider);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace tmcmt::net
