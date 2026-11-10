#pragma once

#include <string>

#include "trafficmcmt/core/Result.h"

namespace tmcmt::net {

/// RAII wrapper around a ZeroMQ context (`zmq_ctx_new` / `zmq_ctx_term`).
///
/// The context is not copyable but is movable. `Shutdown()` is the only method
/// that may be called from a different thread while another thread is blocked
/// in a socket call: it makes every blocking call return `ETERM`, which is how
/// `ZmqTrackletSource::Stop()` unblocks a pending `Next()`.
class ZmqContext {
public:
    ZmqContext() = default;
    ~ZmqContext();

    ZmqContext(const ZmqContext&) = delete;
    ZmqContext& operator=(const ZmqContext&) = delete;

    ZmqContext(ZmqContext&& other) noexcept;
    ZmqContext& operator=(ZmqContext&& other) noexcept;

    /// Creates the context with the given number of I/O threads.
    [[nodiscard]] Status Init(int io_threads = 1);

    /// Interrupts every blocking operation on sockets owned by this context.
    /// Safe to call from a signal/control thread. The context stays valid.
    void Shutdown() noexcept;

    /// Shuts the context down and terminates it. Blocks until all sockets are
    /// closed, so close the sockets first.
    void Close() noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return m_ctx != nullptr;
    }

    /// Raw `void*` context, for code that needs the C API directly.
    [[nodiscard]] void* handle() const noexcept {
        return m_ctx;
    }

private:
    void* m_ctx = nullptr;
};

} // namespace tmcmt::net
