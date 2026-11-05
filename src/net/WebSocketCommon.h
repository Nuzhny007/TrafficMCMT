#pragma once

// Internal helpers shared by the WebSocket server and client.
// Kept header-only and dependency-free: platform sockets + RFC 6455 framing.

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tmcmt::net::detail {

#ifdef _WIN32
using Socket = SOCKET;
using SockLen = int;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
using SockLen = socklen_t;
inline constexpr Socket kInvalidSocket = -1;
#endif

#ifdef _WIN32
inline bool EnsureSockets() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
inline void CloseSocket(Socket socket) {
    if (socket != kInvalidSocket) {
        closesocket(socket);
    }
}
inline int LastSocketError() {
    return WSAGetLastError();
}
#else
inline bool EnsureSockets() {
    return true;
}
inline void CloseSocket(Socket socket) {
    if (socket != kInvalidSocket) {
        ::close(socket);
    }
}
inline int LastSocketError() {
    return errno;
}
#endif

inline int SendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

inline bool SetSocketTimeouts(Socket socket, int receiveMs, int sendMs) {
#ifdef _WIN32
    DWORD receive = static_cast<DWORD>(std::max(0, receiveMs));
    DWORD send = static_cast<DWORD>(std::max(0, sendMs));
    const char* r = reinterpret_cast<const char*>(&receive);
    const char* s = reinterpret_cast<const char*>(&send);
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, r, sizeof(receive)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, s, sizeof(send)) == 0;
#else
    timeval receive{std::max(0, receiveMs) / 1000, (std::max(0, receiveMs) % 1000) * 1000};
    timeval send{std::max(0, sendMs) / 1000, (std::max(0, sendMs) % 1000) * 1000};
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)) == 0 &&
           setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof(send)) == 0;
#endif
}

inline bool SendAll(Socket socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent, 1U << 20));
        const int written = ::send(socket, data.data() + sent, chunk, SendFlags());
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

inline int ReceiveSome(Socket socket, char* buffer, int size) {
    return static_cast<int>(::recv(socket, buffer, size, 0));
}

inline void ShutdownSocket(Socket socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

// ---------------------------------------------------------------------------
// SHA-1 (RFC 3174) and Base64 (RFC 4648) for the WebSocket handshake.
// ---------------------------------------------------------------------------

inline std::uint32_t RotateLeft(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

inline std::array<std::uint8_t, 20> Sha1(std::string_view data) {
    std::uint32_t state[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bitLength = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0U);
    }
    for (int shift = 7; shift >= 0; --shift) {
        message.push_back(static_cast<std::uint8_t>((bitLength >> (shift * 8)) & 0xFFU));
    }

    for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
        std::uint32_t words[80] = {};
        for (int i = 0; i < 16; ++i) {
            const std::size_t base = offset + static_cast<std::size_t>(i) * 4U;
            words[i] = (static_cast<std::uint32_t>(message[base]) << 24U) |
                       (static_cast<std::uint32_t>(message[base + 1]) << 16U) |
                       (static_cast<std::uint32_t>(message[base + 2]) << 8U) |
                       static_cast<std::uint32_t>(message[base + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];

        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t temp = RotateLeft(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = RotateLeft(b, 30);
            b = a;
            a = temp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    std::array<std::uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[static_cast<std::size_t>(i) * 4U] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4U + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4U + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
        digest[static_cast<std::size_t>(i) * 4U + 3] = static_cast<std::uint8_t>(state[i] & 0xFFU);
    }
    return digest;
}

inline std::string Base64Encode(const std::uint8_t* data, std::size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2U) / 3U * 4U);
    for (std::size_t i = 0; i < size; i += 3U) {
        const std::uint32_t first = data[i];
        const std::uint32_t second = (i + 1U < size) ? data[i + 1U] : 0U;
        const std::uint32_t third = (i + 2U < size) ? data[i + 2U] : 0U;
        const std::uint32_t value = (first << 16U) | (second << 8U) | third;

        out.push_back(kTable[(value >> 18U) & 0x3FU]);
        out.push_back(kTable[(value >> 12U) & 0x3FU]);
        out.push_back((i + 1U < size) ? kTable[(value >> 6U) & 0x3FU] : '=');
        out.push_back((i + 2U < size) ? kTable[value & 0x3FU] : '=');
    }
    return out;
}

inline std::string WebSocketAcceptKey(std::string_view clientKey) {
    static constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string input(clientKey);
    input.append(kGuid);
    const std::array<std::uint8_t, 20> digest = Sha1(input);
    return Base64Encode(digest.data(), digest.size());
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

struct Frame {
    bool fin = true;
    std::uint8_t opcode = 0;
    std::string payload;
};

enum class FrameParse {
    Ok,
    NeedMore,
    Error,
};

inline std::array<std::uint8_t, 4> RandomMask() {
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<int> distribution(0, 255);
    std::array<std::uint8_t, 4> mask{};
    for (std::uint8_t& byte : mask) {
        byte = static_cast<std::uint8_t>(distribution(generator));
    }
    return mask;
}

inline std::string EncodeFrame(std::uint8_t opcode, std::string_view payload, bool mask) {
    std::string frame;
    frame.reserve(payload.size() + 14U);
    frame.push_back(static_cast<char>(0x80U | (opcode & 0x0FU)));

    const std::size_t size = payload.size();
    const auto maskBit = static_cast<std::uint8_t>(mask ? 0x80U : 0x00U);
    if (size < 126U) {
        frame.push_back(static_cast<char>(maskBit | static_cast<std::uint8_t>(size)));
    } else if (size <= 0xFFFFU) {
        frame.push_back(static_cast<char>(maskBit | 126U));
        frame.push_back(static_cast<char>((size >> 8U) & 0xFFU));
        frame.push_back(static_cast<char>(size & 0xFFU));
    } else {
        frame.push_back(static_cast<char>(maskBit | 127U));
        for (int shift = 7; shift >= 0; --shift) {
            frame.push_back(static_cast<char>((size >> (shift * 8)) & 0xFFU));
        }
    }

    if (mask) {
        const std::array<std::uint8_t, 4> key = RandomMask();
        for (const std::uint8_t byte : key) {
            frame.push_back(static_cast<char>(byte));
        }
        for (std::size_t i = 0; i < size; ++i) {
            frame.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4U]));
        }
    } else {
        frame.append(payload.data(), payload.size());
    }
    return frame;
}

inline FrameParse ParseFrame(std::string& buffer, std::size_t maxPayload, bool requireMask, Frame& out) {
    if (buffer.size() < 2U) {
        return FrameParse::NeedMore;
    }
    const auto first = static_cast<std::uint8_t>(buffer[0]);
    const auto second = static_cast<std::uint8_t>(buffer[1]);

    if ((first & 0x70U) != 0U) {
        return FrameParse::Error;
    }
    out.fin = (first & 0x80U) != 0U;
    out.opcode = static_cast<std::uint8_t>(first & 0x0FU);

    const bool masked = (second & 0x80U) != 0U;
    if (requireMask && !masked) {
        return FrameParse::Error;
    }

    std::uint64_t length = second & 0x7FU;
    std::size_t offset = 2;
    if (length == 126U) {
        if (buffer.size() < 4U) {
            return FrameParse::NeedMore;
        }
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[2])) << 8U) |
                 static_cast<std::uint8_t>(buffer[3]);
        offset = 4;
    } else if (length == 127U) {
        if (buffer.size() < 10U) {
            return FrameParse::NeedMore;
        }
        length = 0;
        for (int i = 0; i < 8; ++i) {
            length = (length << 8U) | static_cast<std::uint8_t>(buffer[2 + static_cast<std::size_t>(i)]);
        }
        offset = 10;
    }
    if (length > maxPayload) {
        return FrameParse::Error;
    }

    std::array<std::uint8_t, 4> key{0, 0, 0, 0};
    if (masked) {
        if (buffer.size() < offset + 4U) {
            return FrameParse::NeedMore;
        }
        for (std::size_t i = 0; i < 4U; ++i) {
            key[i] = static_cast<std::uint8_t>(buffer[offset + i]);
        }
        offset += 4U;
    }

    if (buffer.size() < offset + length) {
        return FrameParse::NeedMore;
    }

    out.payload.assign(buffer.data() + offset, static_cast<std::size_t>(length));
    if (masked) {
        for (std::size_t i = 0; i < out.payload.size(); ++i) {
            out.payload[i] = static_cast<char>(static_cast<std::uint8_t>(out.payload[i]) ^ key[i % 4U]);
        }
    }
    buffer.erase(0, offset + static_cast<std::size_t>(length));
    return FrameParse::Ok;
}

// ---------------------------------------------------------------------------
// Minimal HTTP request parsing (server handshake and /health).
// ---------------------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
};

inline std::string TrimCopy(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline std::string LowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

inline bool ParseHttpRequest(std::string_view raw, HttpRequest& out) {
    const std::size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string_view::npos) {
        return false;
    }
    const std::string requestLine(raw.substr(0, lineEnd));
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace = requestLine.find(' ', firstSpace == std::string::npos ? 0 : firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        return false;
    }
    out.method = requestLine.substr(0, firstSpace);
    out.path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const std::size_t query = out.path.find('?');
    if (query != std::string::npos) {
        out.path.resize(query);
    }

    std::size_t position = lineEnd + 2;
    while (position < raw.size()) {
        const std::size_t end = raw.find("\r\n", position);
        if (end == std::string_view::npos || end == position) {
            break;
        }
        const std::string line(raw.substr(position, end - position));
        const std::size_t separator = line.find(':');
        if (separator != std::string::npos) {
            out.headers[LowerCopy(TrimCopy(line.substr(0, separator)))] = TrimCopy(line.substr(separator + 1));
        }
        position = end + 2;
    }
    return true;
}

inline bool HeaderContains(const std::map<std::string, std::string>& headers, const std::string& name,
                           const std::string& token) {
    const auto it = headers.find(name);
    if (it == headers.end()) {
        return false;
    }
    return LowerCopy(it->second).find(LowerCopy(token)) != std::string::npos;
}

} // namespace tmcmt::net::detail
