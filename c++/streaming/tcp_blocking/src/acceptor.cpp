/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/tcp_blocking/acceptor.h>

#include <utility>

#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/logger.h>
#else
#  include <arpa/inet.h>
#  include <cstring>
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace streaming::blocking::tcp
{
    acceptor::acceptor(endpoint ep)
        : endpoint_(std::move(ep))
    {
    }

#ifdef CANOPY_BUILD_COROUTINE

    acceptor::acceptor(
        const coro::net::socket_address& endpoint_addr,
        coro::net::tcp::server::options opts)
        : opts_(opts)
    {
        // Extract host/port from the libcoro socket_address for round-tripping.
        endpoint_.host = endpoint_addr.ip().to_string();
        endpoint_.port = endpoint_addr.port();
        endpoint_.ipv6 = endpoint_addr.domain() == coro::net::domain_t::ipv6;
    }

    acceptor::~acceptor() = default;

    bool acceptor::init(std::shared_ptr<rpc::executor> executor)
    {
        executor_ = std::move(executor);
        if (!executor_)
            return false;
        stop_.store(false, std::memory_order_release);
        coro::net::socket_address addr{coro::net::ip_address::from_string(endpoint_.host), endpoint_.port};
        std::shared_ptr<coro::scheduler> sched = executor_;
        server_ = std::make_shared<coro::net::tcp::server>(sched, addr, opts_);
        return true;
    }

    CORO_TASK(std::optional<std::shared_ptr<::streaming::stream>>) acceptor::accept()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            auto client = co_await server_->accept(poll_timeout_);
            if (client)
            {
                CO_RETURN std::make_shared<stream>(std::move(*client), executor_);
            }
            if (client.error().is_timeout())
                continue;
            if (!stop_.load(std::memory_order_acquire))
                RPC_ERROR("tcp::acceptor: accept error");
            break;
        }
        CO_RETURN std::nullopt;
    }

    void acceptor::stop()
    {
        stop_.store(true, std::memory_order_release);
    }

#else // blocking

    namespace
    {
        int create_listen_socket(const endpoint& ep)
        {
            const int family = ep.ipv6 ? AF_INET6 : AF_INET;
            int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return -1;

            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

            if (ep.ipv6)
            {
                sockaddr_in6 addr{};
                addr.sin6_family = AF_INET6;
                addr.sin6_port = htons(ep.port);
                if (::inet_pton(AF_INET6, ep.host.c_str(), &addr.sin6_addr) != 1)
                {
                    ::close(fd);
                    return -1;
                }
                if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
                {
                    ::close(fd);
                    return -1;
                }
            }
            else
            {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(ep.port);
                if (::inet_pton(AF_INET, ep.host.c_str(), &addr.sin_addr) != 1)
                {
                    ::close(fd);
                    return -1;
                }
                if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
                {
                    ::close(fd);
                    return -1;
                }
            }

            if (::listen(fd, SOMAXCONN) != 0)
            {
                ::close(fd);
                return -1;
            }
            return fd;
        }
    }

    acceptor::~acceptor()
    {
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    bool acceptor::init(std::shared_ptr<rpc::executor> executor)
    {
        if (!executor)
            return false;
        stop_.store(false, std::memory_order_release);
        if (listen_fd_ >= 0)
        {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        listen_fd_ = create_listen_socket(endpoint_);
        return listen_fd_ >= 0;
    }

    std::optional<std::shared_ptr<::streaming::stream>> acceptor::accept()
    {
        while (!stop_.load(std::memory_order_acquire))
        {
            if (listen_fd_ < 0)
                return std::nullopt;

            pollfd pfd{};
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            int r = ::poll(&pfd, 1, static_cast<int>(poll_timeout_.count()));
            if (r == 0)
                continue;
            if (r < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pfd.revents & (POLLERR | POLLNVAL))
                break;
            if (!(pfd.revents & POLLIN))
                continue;

            sockaddr_storage peer{};
            socklen_t peer_len = sizeof(peer);
            int client_fd
                = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                if (stop_.load(std::memory_order_acquire))
                    break;
                continue; // transient — try again
            }
            return std::make_shared<stream>(socket(client_fd));
        }
        return std::nullopt;
    }

    void acceptor::stop()
    {
        stop_.store(true, std::memory_order_release);
        // Interrupt any thread currently blocked in poll/accept on listen_fd_
        // by half-shutting the listen fd; subsequent poll wakes with POLLHUP.
        if (listen_fd_ >= 0)
            ::shutdown(listen_fd_, SHUT_RD);
    }

#endif

} // namespace streaming::blocking::tcp
