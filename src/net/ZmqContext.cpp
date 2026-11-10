#include "trafficmcmt/net/ZmqContext.h"

#include <utility>

#include <zmq.h>

namespace tmcmt::net {

ZmqContext::~ZmqContext() {
    Close();
}

ZmqContext::ZmqContext(ZmqContext&& other) noexcept : m_ctx(std::exchange(other.m_ctx, nullptr)) {}

ZmqContext& ZmqContext::operator=(ZmqContext&& other) noexcept {
    if (this != &other) {
        Close();
        m_ctx = std::exchange(other.m_ctx, nullptr);
    }
    return *this;
}

Status ZmqContext::Init(int io_threads) {
    if (m_ctx != nullptr) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ context is already initialized");
    }
    if (io_threads < 1) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ context needs at least one I/O thread");
    }

    m_ctx = zmq_ctx_new();
    if (m_ctx == nullptr) {
        return Status::Error(ErrorCode::Internal,
                             std::string("zmq_ctx_new failed: ") + zmq_strerror(zmq_errno()));
    }

    const int rc = zmq_ctx_set(m_ctx, ZMQ_IO_THREADS, io_threads);
    if (rc != 0) {
        const int err = zmq_errno();
        zmq_ctx_term(m_ctx);
        m_ctx = nullptr;
        return Status::Error(ErrorCode::Internal,
                             std::string("zmq_ctx_set(ZMQ_IO_THREADS) failed: ") + zmq_strerror(err));
    }
    return Status::Ok();
}

void ZmqContext::Shutdown() noexcept {
    if (m_ctx != nullptr) {
        zmq_ctx_shutdown(m_ctx);
    }
}

void ZmqContext::Close() noexcept {
    if (m_ctx != nullptr) {
        zmq_ctx_term(m_ctx);
        m_ctx = nullptr;
    }
}

} // namespace tmcmt::net
