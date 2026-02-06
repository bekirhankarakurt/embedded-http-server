#include "ehs/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace ehs {

namespace {

constexpr std::uint64_t kListenId = 0;
constexpr std::uint64_t kWakeId = 1;
constexpr int kTickMs = 100;
constexpr std::size_t kReadBufferSize = 16384;
constexpr int kMaxReadsPerEvent = 4;
constexpr std::string_view kBusyResponse =
    "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void epoll_control(int epoll_fd, int op, int fd, std::uint32_t events, std::uint64_t id) {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = id;
    if (::epoll_ctl(epoll_fd, op, fd, &ev) != 0) {
        throw_errno("epoll_ctl");
    }
}

}  // namespace

UniqueFd::~UniqueFd() { reset(); }

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int UniqueFd::release() noexcept { return std::exchange(fd_, -1); }

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

struct Server::Connection {
    Connection(std::uint64_t connection_id, UniqueFd socket, const Handler& handler,
               const SessionOptions& options, Clock::time_point now)
        : id(connection_id), fd(std::move(socket)), session(handler, options), last_progress(now),
          request_started(now) {}

    std::uint64_t id;
    UniqueFd fd;
    Session session;
    Clock::time_point last_progress;
    Clock::time_point request_started;
    std::uint32_t interest{0};
    std::uint64_t counted_requests{0};
    bool peer_closed{false};
    bool broken{false};
};

Server::Server(Handler handler, ServerOptions options)
    : handler_(std::move(handler)), options_(std::move(options)),
      epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)), wake_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!epoll_fd_.valid()) {
        throw_errno("epoll_create1");
    }
    if (!wake_fd_.valid()) {
        throw_errno("eventfd");
    }
    epoll_control(epoll_fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), EPOLLIN, kWakeId);
}

Server::~Server() = default;

void Server::listen() {
    sockaddr_storage storage{};
    socklen_t length = 0;
    int family = AF_INET;
    if (options_.bind_address.find(':') != std::string::npos) {
        family = AF_INET6;
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(options_.port);
        if (::inet_pton(AF_INET6, options_.bind_address.c_str(), &addr.sin6_addr) != 1) {
            throw std::invalid_argument("invalid bind address: " + options_.bind_address);
        }
        std::memcpy(&storage, &addr, sizeof(addr));
        length = sizeof(addr);
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(options_.port);
        if (::inet_pton(AF_INET, options_.bind_address.c_str(), &addr.sin_addr) != 1) {
            throw std::invalid_argument("invalid bind address: " + options_.bind_address);
        }
        std::memcpy(&storage, &addr, sizeof(addr));
        length = sizeof(addr);
    }

    UniqueFd fd(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        throw_errno("socket");
    }
    const int enable = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0) {
        throw_errno("setsockopt(SO_REUSEADDR)");
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket API.
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&storage), length) != 0) {
        throw_errno("bind");
    }
    if (::listen(fd.get(), options_.backlog) != 0) {
        throw_errno("listen");
    }
    sockaddr_storage bound{};
    socklen_t bound_length = sizeof(bound);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket API.
    if (::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
        throw_errno("getsockname");
    }
    if (bound.ss_family == AF_INET6) {
        sockaddr_in6 addr{};
        std::memcpy(&addr, &bound, sizeof(addr));
        bound_port_ = ntohs(addr.sin6_port);
    } else {
        sockaddr_in addr{};
        std::memcpy(&addr, &bound, sizeof(addr));
        bound_port_ = ntohs(addr.sin_port);
    }
    epoll_control(epoll_fd_.get(), EPOLL_CTL_ADD, fd.get(), EPOLLIN, kListenId);
    listen_fd_ = std::move(fd);
}

void Server::stop() noexcept {
    const std::uint64_t one = 1;
    const auto written = ::write(wake_fd_.get(), &one, sizeof(one));
    static_cast<void>(written);
}

void Server::run() {
    if (!listen_fd_.valid() && !draining_) {
        listen();
    }
    std::array<epoll_event, 64> events{};
    while (true) {
        const int count = ::epoll_wait(epoll_fd_.get(), events.data(), static_cast<int>(events.size()),
                                       kTickMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("epoll_wait");
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
            const auto id = events.at(i).data.u64;
            if (id == kWakeId) {
                std::uint64_t value = 0;
                const auto got = ::read(wake_fd_.get(), &value, sizeof(value));
                static_cast<void>(got);
                begin_drain();
            } else if (id == kListenId) {
                accept_connections();
            } else {
                handle_event(id, events.at(i).events);
            }
        }
        const auto now = Clock::now();
        sweep(now);
        if (draining_ && (connections_.empty() || now >= drain_deadline_)) {
            break;
        }
    }
    connections_.clear();
    stats_.connections_active = 0;
}

void Server::set_listening(bool enabled) {
    if (!listen_fd_.valid() || listen_paused_ == !enabled) {
        return;
    }
    epoll_control(epoll_fd_.get(), EPOLL_CTL_MOD, listen_fd_.get(), enabled ? EPOLLIN : 0U, kListenId);
    listen_paused_ = !enabled;
}

void Server::accept_connections() {
    while (listen_fd_.valid()) {
        UniqueFd client(::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!client.valid()) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                set_listening(false);
            }
            return;
        }
        if (connections_.size() >= options_.max_connections) {
            const auto sent = ::send(client.get(), kBusyResponse.data(), kBusyResponse.size(),
                                     MSG_NOSIGNAL | MSG_DONTWAIT);
            static_cast<void>(sent);
            ++stats_.connections_rejected;
            continue;
        }
        const int enable = 1;
        static_cast<void>(::setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)));
        const auto id = next_id_++;
        const int fd = client.get();
        auto connection =
            std::make_unique<Connection>(id, std::move(client), handler_, options_.session, Clock::now());
        connection->interest = EPOLLIN | EPOLLRDHUP;
        epoll_control(epoll_fd_.get(), EPOLL_CTL_ADD, fd, connection->interest, id);
        connections_.emplace(id, std::move(connection));
        ++stats_.connections_accepted;
        ++stats_.connections_active;
    }
}

void Server::account(Connection& connection) {
    const auto handled = connection.session.requests_handled();
    stats_.requests_handled += handled - connection.counted_requests;
    connection.counted_requests = handled;
}

void Server::handle_event(std::uint64_t id, std::uint32_t events) {
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    Connection& connection = *it->second;
    if ((events & (EPOLLERR | EPOLLHUP)) != 0U) {
        close_connection(id);
        return;
    }
    if ((events & (EPOLLIN | EPOLLRDHUP)) != 0U) {
        std::array<char, kReadBufferSize> buffer{};
        for (int i = 0; i < kMaxReadsPerEvent && connection.session.wants_input(); ++i) {
            const auto n = ::recv(connection.fd.get(), buffer.data(), buffer.size(), 0);
            if (n > 0) {
                const bool was_in_progress = connection.session.request_in_progress();
                const auto now = Clock::now();
                connection.last_progress = now;
                connection.session.receive(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
                if (!was_in_progress && connection.session.request_in_progress()) {
                    connection.request_started = now;
                }
                if (static_cast<std::size_t>(n) < buffer.size()) {
                    break;
                }
            } else if (n == 0) {
                connection.peer_closed = true;
                connection.session.receive_eof();
                break;
            } else if (errno == EINTR) {
                continue;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    connection.broken = true;
                }
                break;
            }
        }
    }
    flush(connection);
    account(connection);
    if (connection.broken || connection.session.done() ||
        (connection.peer_closed && !connection.session.has_pending_output())) {
        close_connection(id);
        return;
    }
    update_interest(connection);
}

void Server::flush(Connection& connection) {
    while (!connection.broken) {
        auto out = connection.session.output();
        if (out.empty()) {
            connection.session.pump();
            out = connection.session.output();
            if (out.empty()) {
                return;
            }
        }
        const auto n = ::send(connection.fd.get(), out.data(), out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            connection.session.consume(static_cast<std::size_t>(n));
            connection.last_progress = Clock::now();
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            connection.broken = true;
        }
    }
}

void Server::update_interest(Connection& connection) {
    std::uint32_t wanted = 0;
    if (connection.session.wants_input() && !connection.peer_closed) {
        wanted |= EPOLLIN | EPOLLRDHUP;
    }
    if (connection.session.has_pending_output()) {
        wanted |= EPOLLOUT;
    }
    if (wanted != connection.interest) {
        epoll_control(epoll_fd_.get(), EPOLL_CTL_MOD, connection.fd.get(), wanted, connection.id);
        connection.interest = wanted;
    }
}

void Server::close_connection(std::uint64_t id) {
    const auto it = connections_.find(id);
    if (it == connections_.end()) {
        return;
    }
    account(*it->second);
    static_cast<void>(::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, it->second->fd.get(), nullptr));
    connections_.erase(it);
    --stats_.connections_active;
    if (!draining_) {
        set_listening(true);
    }
}

void Server::begin_drain() {
    if (draining_) {
        return;
    }
    draining_ = true;
    drain_deadline_ = Clock::now() + options_.drain_timeout;
    if (listen_fd_.valid()) {
        static_cast<void>(::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, listen_fd_.get(), nullptr));
        listen_fd_.reset();
    }
    std::vector<std::uint64_t> idle;
    for (auto& [id, connection] : connections_) {
        connection->session.finish_after_current();
        flush(*connection);
        if (connection->session.done() || connection->broken) {
            idle.push_back(id);
        } else {
            update_interest(*connection);
        }
    }
    for (const auto id : idle) {
        close_connection(id);
    }
}

void Server::sweep(Clock::time_point now) {
    std::vector<std::uint64_t> expired;
    for (auto& [id, connection] : connections_) {
        Session& session = connection->session;
        if (session.has_pending_output()) {
            if (now - connection->last_progress > options_.write_timeout) {
                expired.push_back(id);
            }
        } else if (session.request_in_progress()) {
            if (now - connection->request_started > options_.request_timeout) {
                session.fail(Status::RequestTimeout);
                flush(*connection);
                expired.push_back(id);
            }
        } else if (now - connection->last_progress > options_.idle_timeout) {
            expired.push_back(id);
        }
    }
    for (const auto id : expired) {
        ++stats_.timeouts;
        close_connection(id);
    }
}

}  // namespace ehs
