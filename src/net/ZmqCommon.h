#pragma once

// Internal ZeroMQ helpers shared by the transport classes.
// The public headers expose only `void*` handles, so `zmq.h` is confined here.

#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zmq.h>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net::detail {

/// Minimal RAII owner of a `zmq_socket`.
class Socket {
public:
    Socket() = default;
    explicit Socket(void* socket) noexcept : m_socket(socket) {}
    ~Socket() {
        Close();
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : m_socket(std::exchange(other.m_socket, nullptr)) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            Close();
            m_socket = std::exchange(other.m_socket, nullptr);
        }
        return *this;
    }

    void Close() noexcept {
        if (m_socket != nullptr) {
            zmq_close(m_socket);
            m_socket = nullptr;
        }
    }

    /// Transfers ownership to the caller without closing the socket.
    [[nodiscard]] void* Release() noexcept {
        return std::exchange(m_socket, nullptr);
    }

    [[nodiscard]] void* get() const noexcept {
        return m_socket;
    }

    [[nodiscard]] bool valid() const noexcept {
        return m_socket != nullptr;
    }

    explicit operator bool() const noexcept {
        return valid();
    }

private:
    void* m_socket = nullptr;
};

inline Status MakeZmqError(std::string_view what) {
    return Status::Error(ErrorCode::IoError, std::string(what) + ": " + zmq_strerror(zmq_errno()));
}

inline Status MakeZmqError(std::string_view what, int err) {
    return Status::Error(ErrorCode::IoError, std::string(what) + ": " + zmq_strerror(err));
}

inline Status SetInt(void* socket, int option, int value) {
    if (zmq_setsockopt(socket, option, &value, sizeof(value)) != 0) {
        return MakeZmqError("zmq_setsockopt");
    }
    return Status::Ok();
}

/// Sets an option and ignores failures; used for optional tuning knobs that
/// older libzmq builds may not support.
inline void TrySetInt(void* socket, int option, int value) noexcept {
    (void)zmq_setsockopt(socket, option, &value, sizeof(value));
}

inline Status SetString(void* socket, int option, std::string_view value) {
    if (zmq_setsockopt(socket, option, value.data(), value.size()) != 0) {
        return MakeZmqError("zmq_setsockopt");
    }
    return Status::Ok();
}

inline Status Bind(void* socket, std::string_view endpoint) {
    const std::string text(endpoint);
    if (zmq_bind(socket, text.c_str()) != 0) {
        return MakeZmqError("zmq_bind " + text);
    }
    return Status::Ok();
}

inline Status Connect(void* socket, std::string_view endpoint) {
    const std::string text(endpoint);
    if (zmq_connect(socket, text.c_str()) != 0) {
        return MakeZmqError("zmq_connect " + text);
    }
    return Status::Ok();
}

inline Result<std::string> LastEndpoint(void* socket) {
    char buffer[256] = {};
    std::size_t size = sizeof(buffer);
    if (zmq_getsockopt(socket, ZMQ_LAST_ENDPOINT, buffer, &size) != 0) {
        return MakeZmqError("zmq_getsockopt(ZMQ_LAST_ENDPOINT)");
    }
    if (size > 0 && buffer[size - 1] == '\0') {
        --size;
    }
    return std::string(buffer, size);
}

/// Sends a multipart message. On failure the partially sent frames are not
/// rolled back (ZeroMQ does not support that); the caller must treat the
/// message as lost.
inline Status SendFrames(void* socket, const std::vector<std::string_view>& frames, int flags) {
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const int frame_flags = flags | (i + 1 < frames.size() ? ZMQ_SNDMORE : 0);
        if (zmq_send(socket, frames[i].data(), frames[i].size(), frame_flags) < 0) {
            return MakeZmqError("zmq_send");
        }
    }
    return Status::Ok();
}

inline Status SendFrames(void* socket, std::string_view topic, std::string_view payload, int flags) {
    const std::vector<std::string_view> frames{topic, payload};
    return SendFrames(socket, frames, flags);
}

/// Receives one multipart message. `flags` applies to the first frame only.
inline Result<std::vector<std::string>> ReceiveFrames(void* socket, int flags, std::size_t max_frames = 8) {
    std::vector<std::string> frames;

    for (;;) {
        zmq_msg_t message;
        if (zmq_msg_init(&message) != 0) {
            return MakeZmqError("zmq_msg_init");
        }

        if (zmq_msg_recv(&message, socket, frames.empty() ? flags : 0) < 0) {
            const int err = zmq_errno();
            zmq_msg_close(&message);
            return MakeZmqError("zmq_msg_recv", err);
        }

        frames.emplace_back(static_cast<const char*>(zmq_msg_data(&message)), zmq_msg_size(&message));
        const bool more = zmq_msg_more(&message) != 0;
        zmq_msg_close(&message);

        if (!more) {
            return frames;
        }
        if (frames.size() >= max_frames) {
            return Status::Error(ErrorCode::ParseError, "multipart message has too many frames");
        }
    }
}

inline bool IsEagain(int err) noexcept {
    return err == EAGAIN;
}

inline bool IsTerminated(int err) noexcept {
    return err == ETERM;
}

} // namespace tmcmt::net::detail
