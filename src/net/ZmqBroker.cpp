#include "trafficmcmt/net/ZmqBroker.h"

#include <atomic>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <zmq.h>

#include "ZmqCommon.h"
#include "trafficmcmt/net/ZmqContext.h"

namespace tmcmt::net {

struct ZmqBroker::Impl {
    ZmqBrokerOptions options;
    ZmqContext context;
    detail::Socket frontend; // XSUB, publishers connect here
    detail::Socket backend;  // XPUB, subscribers connect here
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    ZmqBrokerStats stats;
    std::string frontend_endpoint;
    std::string backend_endpoint;

    void Run();
};

namespace {

Status Forward(detail::Socket& from, detail::Socket& to, std::uint64_t& counter, std::uint64_t& bytes) {
    auto frames = detail::ReceiveFrames(from.get(), 0);
    if (!frames) {
        return frames.status();
    }

    std::vector<std::string_view> views;
    views.reserve(frames.value().size());
    for (const std::string& frame : frames.value()) {
        views.push_back(frame);
        bytes += frame.size();
    }

    Status status = detail::SendFrames(to.get(), views, 0);
    if (status.ok()) {
        ++counter;
    }
    return status;
}

} // namespace

void ZmqBroker::Impl::Run() {
    zmq_pollitem_t items[2] = {};
    items[0].socket = frontend.get();
    items[0].events = ZMQ_POLLIN;
    items[1].socket = backend.get();
    items[1].events = ZMQ_POLLIN;

    while (!stop.load()) {
        const int rc = zmq_poll(items, 2, 100);
        if (rc < 0) {
            const int err = zmq_errno();
            if (err == EINTR) {
                continue;
            }
            if (detail::IsTerminated(err) || stop.load()) {
                break;
            }
            ++stats.errors;
            break;
        }
        if (rc == 0) {
            continue;
        }

        // Publisher -> broker -> subscribers.
        if ((items[0].revents & ZMQ_POLLIN) != 0) {
            const Status status = Forward(frontend, backend, stats.forwarded_to_subscribers, stats.bytes_forwarded);
            if (!status.ok()) {
                ++stats.errors;
                if (stop.load()) {
                    break;
                }
            }
        }

        // Subscriber subscriptions (XPUB control frames) -> XSUB.
        if ((items[1].revents & ZMQ_POLLIN) != 0) {
            const Status status = Forward(backend, frontend, stats.forwarded_to_publishers, stats.bytes_forwarded);
            if (!status.ok()) {
                ++stats.errors;
                if (stop.load()) {
                    break;
                }
            }
        }
    }

    running.store(false);
}

ZmqBroker::ZmqBroker() = default;

ZmqBroker::~ZmqBroker() {
    Stop();
}

Status ZmqBroker::Start(ZmqBrokerOptions options) {
    if (m_impl != nullptr && m_impl->running.load()) {
        return Status::Error(ErrorCode::InvalidArgument, "ZeroMQ broker is already running");
    }
    if (options.frontend_endpoint.empty() || options.backend_endpoint.empty()) {
        return Status::Error(ErrorCode::InvalidArgument, "broker endpoints must not be empty");
    }

    Stop();

    auto impl = std::make_unique<Impl>();
    impl->options = std::move(options);

    Status status = impl->context.Init(1);
    if (!status.ok()) {
        return status;
    }

    impl->frontend = detail::Socket(zmq_socket(impl->context.handle(), ZMQ_XSUB));
    impl->backend = detail::Socket(zmq_socket(impl->context.handle(), ZMQ_XPUB));
    if (!impl->frontend || !impl->backend) {
        status = detail::MakeZmqError("zmq_socket(ZMQ_XSUB/ZMQ_XPUB)");
        impl->frontend.Close();
        impl->backend.Close();
        impl->context.Close();
        return status;
    }

    for (void* socket : {impl->frontend.get(), impl->backend.get()}) {
        if (!(status = detail::SetInt(socket, ZMQ_SNDHWM, impl->options.hwm)).ok() ||
            !(status = detail::SetInt(socket, ZMQ_RCVHWM, impl->options.hwm)).ok() ||
            !(status = detail::SetInt(socket, ZMQ_LINGER, impl->options.linger_ms)).ok()) {
            impl->frontend.Close();
            impl->backend.Close();
            impl->context.Close();
            return status;
        }
    }

    if (!(status = detail::Bind(impl->frontend.get(), impl->options.frontend_endpoint)).ok()) {
        impl->frontend.Close();
        impl->backend.Close();
        impl->context.Close();
        return status;
    }
    if (!(status = detail::Bind(impl->backend.get(), impl->options.backend_endpoint)).ok()) {
        impl->frontend.Close();
        impl->backend.Close();
        impl->context.Close();
        return status;
    }

    auto frontend_endpoint = detail::LastEndpoint(impl->frontend.get());
    auto backend_endpoint = detail::LastEndpoint(impl->backend.get());
    if (!frontend_endpoint || !backend_endpoint) {
        status = frontend_endpoint ? backend_endpoint.status() : frontend_endpoint.status();
        impl->frontend.Close();
        impl->backend.Close();
        impl->context.Close();
        return status;
    }
    impl->frontend_endpoint = std::move(frontend_endpoint).value();
    impl->backend_endpoint = std::move(backend_endpoint).value();

    impl->stop.store(false);
    impl->running.store(true);
    impl->thread = std::thread([raw = impl.get()] { raw->Run(); });

    m_impl = std::move(impl);
    return Status::Ok();
}

void ZmqBroker::Stop() noexcept {
    if (m_impl == nullptr) {
        return;
    }

    m_impl->stop.store(true);
    m_impl->context.Shutdown();
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }

    m_impl->frontend.Close();
    m_impl->backend.Close();
    m_impl->context.Close();
    m_impl->running.store(false);
    m_impl.reset();
}

bool ZmqBroker::running() const noexcept {
    return m_impl != nullptr && m_impl->running.load();
}

std::string ZmqBroker::frontend_endpoint() const {
    return m_impl == nullptr ? std::string{} : m_impl->frontend_endpoint;
}

std::string ZmqBroker::backend_endpoint() const {
    return m_impl == nullptr ? std::string{} : m_impl->backend_endpoint;
}

const ZmqBrokerStats& ZmqBroker::stats() const noexcept {
    static const ZmqBrokerStats kEmpty{};
    return m_impl == nullptr ? kEmpty : m_impl->stats;
}

} // namespace tmcmt::net
