#include "trafficmcmt/net/WebSocketClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "WebSocketCommon.h"

namespace tmcmt::net {
namespace {

using detail::Frame;
using detail::FrameParse;
using detail::HttpRequest;
using detail::Socket;

constexpr std::uint8_t kOpcodeText = 0x1;
constexpr std::uint8_t kOpcodeClose = 0x8;
constexpr std::uint8_t kOpcodePing = 0x9;
constexpr std::uint8_t kOpcodePong = 0xA;

std::string MakeClientKey() {
    std::array<std::uint8_t, 16> bytes{};
    const std::array<std::uint8_t, 4> mask = detail::RandomMask();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = mask[i % mask.size()] ^ static_cast<std::uint8_t>(i * 37U + 11U);
    }
    return detail::Base64Encode(bytes.data(), bytes.size());
}

} // namespace

struct WebSocketClient::Impl {
    Socket socket = detail::kInvalidSocket;
    std::string buffer;

    bool ReadHttpResponse(std::string& raw) {
        char chunk[4096];
        while (raw.find("\r\n\r\n") == std::string::npos) {
            const int received = detail::ReceiveSome(socket, chunk, static_cast<int>(sizeof(chunk)));
            if (received <= 0) {
                return false;
            }
            raw.append(chunk, static_cast<std::size_t>(received));
            if (raw.size() > 32U * 1024U) {
                return false;
            }
        }
        const std::size_t headerEnd = raw.find("\r\n\r\n");
        buffer = raw.substr(headerEnd + 4U);
        raw.resize(headerEnd);
        return true;
    }
};

WebSocketClient::WebSocketClient(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

WebSocketClient::~WebSocketClient() {
    Close();
}

Result<std::unique_ptr<WebSocketClient>> WebSocketClient::Connect(const std::string& host, std::uint16_t port,
                                                                  const std::string& path, int timeout_ms) {
    if (!detail::EnsureSockets()) {
        return Status::Error(ErrorCode::Internal, "cannot initialise platform sockets");
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portText.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "cannot resolve websocket host " + host);
    }

    Socket socket = detail::kInvalidSocket;
    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == detail::kInvalidSocket) {
            continue;
        }
        if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }
        detail::CloseSocket(socket);
        socket = detail::kInvalidSocket;
    }
    ::freeaddrinfo(resolved);

    if (socket == detail::kInvalidSocket) {
        return Status::Error(ErrorCode::IoError, "cannot connect to ws://" + host + ":" + portText);
    }

    detail::SetSocketTimeouts(socket, timeout_ms, timeout_ms);

    const std::string key = MakeClientKey();
    std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + portText +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!detail::SendAll(socket, request)) {
        detail::CloseSocket(socket);
        return Status::Error(ErrorCode::IoError, "cannot send websocket handshake");
    }

    auto impl = std::make_unique<Impl>();
    impl->socket = socket;

    std::string response;
    if (!impl->ReadHttpResponse(response)) {
        detail::CloseSocket(socket);
        return Status::Error(ErrorCode::IoError, "cannot read websocket handshake response");
    }
    if (response.find(" 101 ") == std::string::npos) {
        detail::CloseSocket(socket);
        return Status::Error(ErrorCode::IoError, "websocket handshake rejected: " + response);
    }

    detail::SetSocketTimeouts(socket, 0, timeout_ms);
    return std::unique_ptr<WebSocketClient>(new WebSocketClient(std::move(impl)));
}

Status WebSocketClient::SendText(std::string_view text) {
    if (!m_impl || m_impl->socket == detail::kInvalidSocket) {
        return Status::Error(ErrorCode::InvalidArgument, "websocket client is not connected");
    }
    const std::string frame = detail::EncodeFrame(kOpcodeText, text, true);
    if (!detail::SendAll(m_impl->socket, frame)) {
        return Status::Error(ErrorCode::IoError, "cannot send websocket frame");
    }
    return Status::Ok();
}

Result<std::string> WebSocketClient::ReceiveText(int timeout_ms) {
    if (!m_impl || m_impl->socket == detail::kInvalidSocket) {
        return Status::Error(ErrorCode::InvalidArgument, "websocket client is not connected");
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        Frame frame;
        const FrameParse parsed = detail::ParseFrame(m_impl->buffer, 8U << 20, false, frame);
        if (parsed == FrameParse::Ok) {
            if (frame.opcode == kOpcodeText) {
                return frame.payload;
            }
            if (frame.opcode == kOpcodePing) {
                detail::SendAll(m_impl->socket, detail::EncodeFrame(kOpcodePong, frame.payload, true));
                continue;
            }
            if (frame.opcode == kOpcodeClose) {
                Close();
                return Status::Error(ErrorCode::IoError, "websocket closed by peer");
            }
            continue;
        }
        if (parsed == FrameParse::Error) {
            return Status::Error(ErrorCode::ParseError, "malformed websocket frame");
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return Status::Error(ErrorCode::IoError, "websocket receive timeout");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_impl->socket, &readSet);
        timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};

#ifdef _WIN32
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(m_impl->socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            if (ready < 0) {
                return Status::Error(ErrorCode::IoError, "websocket select failed");
            }
            continue;
        }

        char chunk[8192];
        const int received = detail::ReceiveSome(m_impl->socket, chunk, static_cast<int>(sizeof(chunk)));
        if (received <= 0) {
            return Status::Error(ErrorCode::IoError, "websocket connection closed");
        }
        m_impl->buffer.append(chunk, static_cast<std::size_t>(received));
    }
}

void WebSocketClient::Close() {
    if (!m_impl || m_impl->socket == detail::kInvalidSocket) {
        return;
    }
    detail::SendAll(m_impl->socket, detail::EncodeFrame(kOpcodeClose, {}, true));
    detail::ShutdownSocket(m_impl->socket);
    detail::CloseSocket(m_impl->socket);
    m_impl->socket = detail::kInvalidSocket;
}

bool WebSocketClient::Connected() const noexcept {
    return m_impl && m_impl->socket != detail::kInvalidSocket;
}

} // namespace tmcmt::net
