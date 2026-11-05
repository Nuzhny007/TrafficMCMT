#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>

#include "trafficmcmt/net/WebSocketClient.h"
#include "trafficmcmt/net/WebSocketServer.h"

#include "WebSocketCommon.h"

namespace {

using tmcmt::net::WebSocketClient;
using tmcmt::net::WebSocketServer;
using tmcmt::net::WebSocketServerOptions;

WebSocketServerOptions MakeOptions() {
    WebSocketServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;
    options.path = "/ws";
    return options;
}

/// Minimal blocking HTTP GET, used to exercise the `/health` endpoint without
/// pulling an HTTP client into the test binary.
std::string HttpGet(std::uint16_t port, const std::string& path) {
    namespace detail = tmcmt::net::detail;
    detail::EnsureSockets();

    const detail::Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == detail::kInvalidSocket) {
        return {};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        detail::CloseSocket(socket);
        return {};
    }

    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    detail::SendAll(socket, request);

    std::string response;
    char buffer[4096];
    while (true) {
        const int received = detail::ReceiveSome(socket, buffer, static_cast<int>(sizeof(buffer)));
        if (received <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(received));
    }
    detail::CloseSocket(socket);
    return response;
}

TEST(WebSocket, StartsOnEphemeralPortAndStops) {
    WebSocketServer server(MakeOptions());
    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();
    EXPECT_TRUE(server.Running());
    EXPECT_NE(server.Port(), 0);

    server.Stop();
    EXPECT_FALSE(server.Running());
    EXPECT_EQ(server.ClientCount(), 0U);
}

TEST(WebSocket, HandshakeAndTargetedSend) {
    WebSocketServer server(MakeOptions());
    std::promise<std::uint64_t> connected;
    auto connectedFuture = connected.get_future();
    server.SetClientConnectedHandler([&connected](std::uint64_t id) { connected.set_value(id); });

    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    auto client = WebSocketClient::Connect("127.0.0.1", server.Port(), "/ws");
    ASSERT_TRUE(client) << client.status().ToString();

    const std::uint64_t id = connectedFuture.get();
    ASSERT_TRUE(server.SendTo(id, "hello"));

    auto message = client.value()->ReceiveText(3000);
    ASSERT_TRUE(message) << message.status().ToString();
    EXPECT_EQ(message.value(), "hello");
}

TEST(WebSocket, OnConnectFramesArriveInOrder) {
    WebSocketServer server(MakeOptions());
    server.SetClientConnectedHandler([&server](std::uint64_t id) {
        server.SendTo(id, "first");
        server.SendTo(id, "second");
    });

    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    auto client = WebSocketClient::Connect("127.0.0.1", server.Port(), "/ws");
    ASSERT_TRUE(client) << client.status().ToString();

    auto first = client.value()->ReceiveText(3000);
    ASSERT_TRUE(first) << first.status().ToString();
    EXPECT_EQ(first.value(), "first");

    auto second = client.value()->ReceiveText(3000);
    ASSERT_TRUE(second) << second.status().ToString();
    EXPECT_EQ(second.value(), "second");
}

TEST(WebSocket, BroadcastReachesEveryClient) {
    WebSocketServer server(MakeOptions());
    std::mutex mutex;
    std::condition_variable condition;
    int count = 0;
    server.SetClientConnectedHandler([&](std::uint64_t) {
        std::lock_guard<std::mutex> lock(mutex);
        ++count;
        condition.notify_all();
    });

    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    auto first = WebSocketClient::Connect("127.0.0.1", server.Port(), "/ws");
    auto second = WebSocketClient::Connect("127.0.0.1", server.Port(), "/ws");
    ASSERT_TRUE(first) << first.status().ToString();
    ASSERT_TRUE(second) << second.status().ToString();

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(3), [&] { return count >= 2; }));
    }

    server.Broadcast("to-all");

    auto firstMessage = first.value()->ReceiveText(3000);
    auto secondMessage = second.value()->ReceiveText(3000);
    ASSERT_TRUE(firstMessage) << firstMessage.status().ToString();
    ASSERT_TRUE(secondMessage) << secondMessage.status().ToString();
    EXPECT_EQ(firstMessage.value(), "to-all");
    EXPECT_EQ(secondMessage.value(), "to-all");
}

TEST(WebSocket, HealthEndpointReturnsJson) {
    WebSocketServer server(MakeOptions());
    server.SetHealthProvider([] { return std::string("{\"status\":\"ok\",\"protocol\":1,\"active_tracks\":3}"); });

    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    const std::string response = HttpGet(server.Port(), "/health");
    EXPECT_NE(response.find("200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("\"active_tracks\":3"), std::string::npos) << response;
}

TEST(WebSocket, UnknownPathReturns404) {
    WebSocketServer server(MakeOptions());
    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    const std::string response = HttpGet(server.Port(), "/nope");
    EXPECT_NE(response.find("404 Not Found"), std::string::npos) << response;
}

TEST(WebSocket, PlainGetOnEndpointRequiresUpgrade) {
    WebSocketServer server(MakeOptions());
    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    const std::string response = HttpGet(server.Port(), "/ws");
    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
}

TEST(WebSocket, StopDisconnectsClientsWithoutHanging) {
    WebSocketServer server(MakeOptions());
    std::promise<void> connected;
    auto connectedFuture = connected.get_future();
    server.SetClientConnectedHandler([&connected](std::uint64_t) { connected.set_value(); });

    const auto status = server.Start();
    ASSERT_TRUE(status) << status.ToString();

    auto client = WebSocketClient::Connect("127.0.0.1", server.Port(), "/ws");
    ASSERT_TRUE(client) << client.status().ToString();
    connectedFuture.wait();

    server.Stop();
    EXPECT_FALSE(server.Running());

    // The client should observe the closed connection rather than block.
    auto message = client.value()->ReceiveText(1000);
    EXPECT_FALSE(message);
}

} // namespace
