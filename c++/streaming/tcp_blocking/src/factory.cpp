/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/tcp_blocking/factory.h>

#include <chrono>

#include <canopy/dns_resolver/resolver.h>
#include <streaming/tcp_blocking/socket.h>
#include <streaming/tcp_blocking/stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/net/tcp/client.hpp>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace rpc::tcp_blocking
{
    namespace
    {
#ifndef CANOPY_BUILD_COROUTINE
        int connect_socket_blocking(
            const canopy::dns_resolver::endpoint& endpoint,
            std::chrono::milliseconds timeout)
        {
            const int family = endpoint.family == canopy::dns_resolver::address_family::ipv6 ? AF_INET6 : AF_INET;
            int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return -1;

            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            sockaddr_storage storage{};
            socklen_t storage_size = 0;
            if (!canopy::dns_resolver::sockaddr_from_endpoint(endpoint, storage, storage_size))
            {
                ::close(fd);
                return -1;
            }

            int result = ::connect(fd, reinterpret_cast<sockaddr*>(&storage), storage_size);
            if (result == 0)
                return fd;
            if (errno != EINPROGRESS)
            {
                ::close(fd);
                return -1;
            }

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int timeout_ms = timeout.count() > 0 ? static_cast<int>(timeout.count()) : -1;
            result = ::poll(&pfd, 1, timeout_ms);
            if (result <= 0)
            {
                ::close(fd);
                return -1;
            }

            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) != 0 || socket_error != 0)
            {
                ::close(fd);
                return -1;
            }
            return fd;
        }
#endif
    } // namespace

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto executor = service && service->get_executor() ? service->get_executor() : rpc::make_executor();
        if (!executor)
            CO_RETURN rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

        const auto timeout = std::chrono::milliseconds(endpoint.connect_timeout);
        const auto resolved = CO_AWAIT canopy::dns_resolver::resolve_host(
            endpoint.host, endpoint.port, canopy::dns_resolver::make_stream_resolve_options(endpoint.ipv6, timeout));
        if (resolved.error_code != rpc::error::OK() || resolved.endpoints.empty())
            CO_RETURN rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

        for (const auto& resolved_endpoint : resolved.endpoints)
        {
            coro::net::tcp::client client(
                executor, coro::net::socket_address{resolved_endpoint.numeric_host, resolved_endpoint.port});
            auto status = CO_AWAIT client.connect(timeout);
            if (status == coro::net::connect_status::connected)
            {
                CO_RETURN rpc::stream_transport::stream_result{rpc::error::OK(),
                    std::make_shared<::streaming::blocking::tcp::stream>(std::move(client), std::move(executor))};
            }
        }
        CO_RETURN rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};
#else
        const auto timeout = std::chrono::milliseconds(endpoint.connect_timeout);
        const auto resolved = canopy::dns_resolver::resolve_host_blocking(
            endpoint.host, endpoint.port, canopy::dns_resolver::make_stream_resolve_options(endpoint.ipv6, timeout));
        if (resolved.error_code != rpc::error::OK() || resolved.endpoints.empty())
            return rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

        for (const auto& resolved_endpoint : resolved.endpoints)
        {
            const int fd = connect_socket_blocking(resolved_endpoint, timeout);
            if (fd >= 0)
            {
                return rpc::stream_transport::stream_result{rpc::error::OK(),
                    std::make_shared<::streaming::blocking::tcp::stream>(::streaming::blocking::tcp::socket(fd))};
            }
        }
        return rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};
#endif
    }

    std::shared_ptr<::streaming::stream_acceptor> make_acceptor(const ::rpc::tcp_blocking_stream::endpoint& endpoint)
    {
        return std::make_shared<::streaming::blocking::tcp::acceptor>(endpoint);
    }

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN rpc::stream_transport::accept_streams(
            make_acceptor(endpoint),
            std::move(callback),
            rpc::stream_transport::make_connection_settings(),
            std::move(service),
            {},
            endpoint.port);
    }
} // namespace rpc::tcp_blocking
