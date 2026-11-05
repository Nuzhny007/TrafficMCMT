#include "trafficmcmt/net/WebSocketServer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
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
constexpr std::uint8_t kOpcodeBinary = 0x2;
constexpr std::uint8_t kOpcodeClose = 0x8;
constexpr std::uint8_t kOpcodePing = 0x9;
constexpr std::uint8_t kOpcodePong = 0xA;

struct Client {
    Socket socket = detail::kInvalidSocket;
    std::uint64_t id = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> finished{false};

    std::mutex mutex;
    std::deque<std::string> outbox;

    std::thread thread;
    std::string buffer;
    std::string fragment;
    std::uint8_t fragmentOpcode = 0;
};

} // namespace

struct WebSocketServer::Impl {
    explicit Impl(WebSocketServerOptions options) : options(std::move(options)) {}

    WebSocketServerOptions options;
    std::atomic<bool> running{false};
    Socket listenSocket = detail::kInvalidSocket;
    std::thread acceptThread;
    std::uint16_t boundPort = 0;

    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Client>> clients;
    std::uint64_t nextClientId = 1;

    ClientHandler onConnect;
    ClientHandler onDisconnect;
    TextHandler onText;
    HealthProvider health;

    [[nodiscard]] std::string HealthJson() const {
        if (health) {
            return health();
        }
        return "{\"status\":\"ok\",\"protocol\":1}";
    }

    void AcceptLoop();
    void ClientLoop(const std::shared_ptr<Client>& client);
    void RunClient(const std::shared_ptr<Client>& client);
    void ReapFinished();

    bool ReadHandshake(const std::shared_ptr<Client>& client, std::string& raw);
    bool SendHandshakeResponse(Socket socket, const std::string& response);
    bool ProcessFrames(const std::shared_ptr<Client>& client);
    void FlushOutbox(const std::shared_ptr<Client>& client);
    void Enqueue(const std::shared_ptr<Client>& client, std::uint8_t opcode, std::string_view payload);
    void EnqueueText(const std::shared_ptr<Client>& client, std::string_view text);
    void Disconnect(const std::shared_ptr<Client>& client);
    void SendHttp(Socket socket, int status, std::string_view reason, std::string_view body);
};

bool WebSocketServer::Impl::SendHandshakeResponse(Socket socket, const std::string& response) {
    return detail::SendAll(socket, response);
}

void WebSocketServer::Impl::SendHttp(Socket socket, int status, std::string_view reason, std::string_view body) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                           "\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    response.append(body);
    detail::SendAll(socket, response);
}

bool WebSocketServer::Impl::ReadHandshake(const std::shared_ptr<Client>& client, std::string& raw) {
    char buffer[4096];
    constexpr std::size_t kMaxHeaderBytes = 16U * 1024U;
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const int received = detail::ReceiveSome(client->socket, buffer, static_cast<int>(sizeof(buffer)));
        if (received <= 0) {
            return false;
        }
        raw.append(buffer, static_cast<std::size_t>(received));
        if (raw.size() > kMaxHeaderBytes) {
            return false;
        }
    }
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    client->buffer = raw.substr(headerEnd + 4U);
    raw.resize(headerEnd);
    return true;
}

void WebSocketServer::Impl::Enqueue(const std::shared_ptr<Client>& client, std::uint8_t opcode,
                                    std::string_view payload) {
    std::string frame = detail::EncodeFrame(opcode, payload, false);
    std::lock_guard<std::mutex> lock(client->mutex);
    if (client->outbox.size() >= options.max_pending_frames) {
        client->outbox.pop_front();
    }
    client->outbox.push_back(std::move(frame));
}

void WebSocketServer::Impl::EnqueueText(const std::shared_ptr<Client>& client, std::string_view text) {
    Enqueue(client, kOpcodeText, text);
}

void WebSocketServer::Impl::FlushOutbox(const std::shared_ptr<Client>& client) {
    std::deque<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(client->mutex);
        pending.swap(client->outbox);
    }
    for (const std::string& frame : pending) {
        if (!detail::SendAll(client->socket, frame)) {
            client->alive = false;
            return;
        }
    }
}

bool WebSocketServer::Impl::ProcessFrames(const std::shared_ptr<Client>& client) {
    while (true) {
        Frame frame;
        const FrameParse result =
            detail::ParseFrame(client->buffer, options.max_frame_bytes, true, frame);
        if (result == FrameParse::NeedMore) {
            return true;
        }
        if (result == FrameParse::Error) {
            return false;
        }

        switch (frame.opcode) {
        case kOpcodeText:
            if (frame.fin) {
                if (onText) {
                    onText(client->id, frame.payload);
                }
            } else {
                client->fragment = std::move(frame.payload);
                client->fragmentOpcode = kOpcodeText;
            }
            break;
        case 0x0:
            if (client->fragmentOpcode != kOpcodeText) {
                return false;
            }
            client->fragment += frame.payload;
            if (frame.fin) {
                if (onText) {
                    onText(client->id, client->fragment);
                }
                client->fragment.clear();
                client->fragmentOpcode = 0;
            }
            break;
        case kOpcodeClose:
            detail::SendAll(client->socket, detail::EncodeFrame(kOpcodeClose, {}, false));
            return false;
        case kOpcodePing:
            Enqueue(client, kOpcodePong, frame.payload);
            break;
        case kOpcodePong:
            break;
        default:
            return false;
        }
    }
}

void WebSocketServer::Impl::Disconnect(const std::shared_ptr<Client>& client) {
    client->alive = false;
    if (onDisconnect) {
        onDisconnect(client->id);
    }
}

void WebSocketServer::Impl::ClientLoop(const std::shared_ptr<Client>& client) {
    RunClient(client);
    client->finished = true;
}

void WebSocketServer::Impl::RunClient(const std::shared_ptr<Client>& client) {
    detail::SetSocketTimeouts(client->socket, 5000, 2000);

    std::string raw;
    if (!ReadHandshake(client, raw)) {
        Disconnect(client);
        return;
    }

    HttpRequest request;
    if (!detail::ParseHttpRequest(raw, request)) {
        SendHttp(client->socket, 400, "Bad Request", "{\"error\":\"malformed request\"}");
        Disconnect(client);
        return;
    }

    if (request.method == "GET" && request.path == "/health") {
        SendHttp(client->socket, 200, "OK", HealthJson());
        Disconnect(client);
        return;
    }

    if (request.path != options.path) {
        SendHttp(client->socket, 404, "Not Found", "{\"error\":\"not found\"}");
        Disconnect(client);
        return;
    }

    const auto keyIt = request.headers.find("sec-websocket-key");
    const bool upgrade = detail::HeaderContains(request.headers, "upgrade", "websocket");
    const bool connection = detail::HeaderContains(request.headers, "connection", "upgrade");
    if (!upgrade || !connection || keyIt == request.headers.end() || keyIt->second.empty()) {
        SendHttp(client->socket, 400, "Bad Request", "{\"error\":\"websocket upgrade required\"}");
        Disconnect(client);
        return;
    }

    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           detail::WebSocketAcceptKey(keyIt->second) + "\r\n\r\n";
    if (!SendHandshakeResponse(client->socket, response)) {
        Disconnect(client);
        return;
    }

    detail::SetSocketTimeouts(client->socket, 0, 2000);

    if (onConnect) {
        onConnect(client->id);
    }

    while (running && client->alive) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(client->socket, &readSet);
        timeval timeout{0, 50'000};

#ifdef _WIN32
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(client->socket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0 && FD_ISSET(client->socket, &readSet)) {
            char buffer[8192];
            const int received = detail::ReceiveSome(client->socket, buffer, static_cast<int>(sizeof(buffer)));
            if (received <= 0) {
                break;
            }
            client->buffer.append(buffer, static_cast<std::size_t>(received));
            if (!ProcessFrames(client)) {
                break;
            }
        } else if (ready < 0) {
            if (!running) {
                break;
            }
        }

        FlushOutbox(client);
    }

    Disconnect(client);
}

void WebSocketServer::Impl::AcceptLoop() {
    while (running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);
        timeval timeout{0, 100'000};

#ifdef _WIN32
        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = ::select(listenSocket + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready > 0 && FD_ISSET(listenSocket, &readSet)) {
            sockaddr_in address{};
            detail::SockLen length = static_cast<detail::SockLen>(sizeof(address));
            const Socket socket = ::accept(listenSocket, reinterpret_cast<sockaddr*>(&address), &length);
            if (socket != detail::kInvalidSocket) {
                bool atCapacity = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    atCapacity = clients.size() >= options.max_clients;
                }
                if (atCapacity) {
                    detail::CloseSocket(socket);
                    ReapFinished();
                    continue;
                }

                auto client = std::make_shared<Client>();
                client->socket = socket;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    client->id = nextClientId++;
                    // Register before the client thread can invoke `onConnect`,
                    // otherwise an immediate SendTo() would miss the client.
                    clients[client->id] = client;
                }
                client->thread = std::thread(&Impl::ClientLoop, this, client);
            }
        }

        ReapFinished();
    }
}

void WebSocketServer::Impl::ReapFinished() {
    std::vector<std::shared_ptr<Client>> finished;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = clients.begin(); it != clients.end();) {
            if (it->second->finished) {
                finished.push_back(it->second);
                it = clients.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& client : finished) {
        detail::CloseSocket(client->socket);
        if (client->thread.joinable()) {
            client->thread.join();
        }
    }
}

WebSocketServer::WebSocketServer(WebSocketServerOptions options)
    : m_impl(std::make_unique<Impl>(std::move(options))) {}

WebSocketServer::~WebSocketServer() {
    Stop();
}

Status WebSocketServer::Start() {
    if (m_impl->running) {
        return Status::Error(ErrorCode::InvalidArgument, "websocket server is already running");
    }
    if (!detail::EnsureSockets()) {
        return Status::Error(ErrorCode::Internal, "cannot initialise platform sockets");
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* resolved = nullptr;
    const std::string portText = std::to_string(m_impl->options.port);
    if (::getaddrinfo(m_impl->options.host.c_str(), portText.c_str(), &hints, &resolved) != 0 ||
        resolved == nullptr) {
        return Status::Error(ErrorCode::InvalidArgument,
                             "cannot resolve websocket bind address " + m_impl->options.host);
    }

    Socket socket = detail::kInvalidSocket;
    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == detail::kInvalidSocket) {
            continue;
        }
        int reuse = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            ::listen(socket, SOMAXCONN) == 0) {
            break;
        }
        detail::CloseSocket(socket);
        socket = detail::kInvalidSocket;
    }
    ::freeaddrinfo(resolved);

    if (socket == detail::kInvalidSocket) {
        return Status::Error(ErrorCode::IoError,
                             "cannot bind websocket server to " + m_impl->options.host + ":" + portText);
    }

    sockaddr_in bound{};
    detail::SockLen boundLength = static_cast<detail::SockLen>(sizeof(bound));
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
        m_impl->boundPort = ntohs(bound.sin_port);
    } else {
        m_impl->boundPort = m_impl->options.port;
    }

    m_impl->listenSocket = socket;
    m_impl->running = true;
    m_impl->acceptThread = std::thread(&Impl::AcceptLoop, m_impl.get());
    return Status::Ok();
}

void WebSocketServer::Stop() {
    if (!m_impl) {
        return;
    }
    if (!m_impl->running.exchange(false)) {
        return;
    }

    // Let the accept loop notice `running == false` before closing the socket
    // it is selecting on.
    if (m_impl->acceptThread.joinable()) {
        m_impl->acceptThread.join();
    }

    detail::ShutdownSocket(m_impl->listenSocket);
    detail::CloseSocket(m_impl->listenSocket);
    m_impl->listenSocket = detail::kInvalidSocket;

    std::vector<std::shared_ptr<Client>> pending;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        pending.reserve(m_impl->clients.size());
        for (auto& [id, client] : m_impl->clients) {
            (void)id;
            pending.push_back(client);
        }
        m_impl->clients.clear();
    }
    for (const auto& client : pending) {
        client->alive = false;
        detail::ShutdownSocket(client->socket);
        if (client->thread.joinable()) {
            client->thread.join();
        }
        detail::CloseSocket(client->socket);
    }
}

bool WebSocketServer::Running() const noexcept {
    return m_impl && m_impl->running;
}

std::uint16_t WebSocketServer::Port() const noexcept {
    return m_impl ? m_impl->boundPort : 0;
}

std::size_t WebSocketServer::ClientCount() const noexcept {
    if (!m_impl) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->clients.size();
}

void WebSocketServer::Broadcast(std::string_view text) {
    if (!m_impl || !m_impl->running) {
        return;
    }
    std::vector<std::shared_ptr<Client>> targets;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        targets.reserve(m_impl->clients.size());
        for (const auto& [id, client] : m_impl->clients) {
            (void)id;
            targets.push_back(client);
        }
    }
    for (const auto& client : targets) {
        m_impl->EnqueueText(client, text);
    }
}

bool WebSocketServer::SendTo(std::uint64_t client_id, std::string_view text) {
    if (!m_impl) {
        return false;
    }
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const auto it = m_impl->clients.find(client_id);
        if (it == m_impl->clients.end()) {
            return false;
        }
        client = it->second;
    }
    m_impl->EnqueueText(client, text);
    return true;
}

void WebSocketServer::SetClientConnectedHandler(ClientHandler handler) {
    m_impl->onConnect = std::move(handler);
}

void WebSocketServer::SetClientDisconnectedHandler(ClientHandler handler) {
    m_impl->onDisconnect = std::move(handler);
}

void WebSocketServer::SetTextHandler(TextHandler handler) {
    m_impl->onText = std::move(handler);
}

void WebSocketServer::SetHealthProvider(HealthProvider provider) {
    m_impl->health = std::move(provider);
}

} // namespace tmcmt::net
