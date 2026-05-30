/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/tcp_blocking.h>

#include <chrono>
#include <exception>

#include <json/config_loader.h>
#include <json/convert.h>
#include <streaming/tcp_blocking/socket.h>
#include <streaming/tcp_blocking/stream.h>
#include <tcp_blocking_stream/tcp_blocking_stream_config_schema.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/net/tcp/client.hpp>
#else
#  include <arpa/inet.h>
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
            const ::rpc::tcp_blocking_stream::endpoint& endpoint,
            std::chrono::milliseconds timeout)
        {
            const int family = endpoint.ipv6 ? AF_INET6 : AF_INET;
            int fd = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0)
                return -1;

            int flags = ::fcntl(fd, F_GETFL, 0);
            if (flags >= 0)
                ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            sockaddr_storage storage{};
            socklen_t storage_size = 0;
            if (endpoint.ipv6)
            {
                auto* addr = reinterpret_cast<sockaddr_in6*>(&storage);
                addr->sin6_family = AF_INET6;
                addr->sin6_port = htons(endpoint.port);
                if (::inet_pton(AF_INET6, endpoint.host.c_str(), &addr->sin6_addr) != 1)
                {
                    ::close(fd);
                    return -1;
                }
                storage_size = sizeof(sockaddr_in6);
            }
            else
            {
                auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
                addr->sin_family = AF_INET;
                addr->sin_port = htons(endpoint.port);
                if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr->sin_addr) != 1)
                {
                    ::close(fd);
                    return -1;
                }
                storage_size = sizeof(sockaddr_in);
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

    const json::v1::object& tcp_blocking_default_options()
    {
        static const json::v1::object options = []
        {
            const ::rpc::tcp_blocking_stream::endpoint defaults;
            using json::v1::convert::to_json_object;
            return to_json_object(defaults);
        }();
        return options;
    }

    const json::v1::object& tcp_blocking_options_schema()
    {
        static const json::v1::object schema
            = json::v1::parse(::rpc::tcp_blocking_stream::endpoint::get_schema(rpc::encoding::yas_json));
        return schema;
    }

    materialise_tcp_blocking_options_result materialise_tcp_blocking_options(const json::v1::object& client_options)
    {
        try
        {
            return {rpc::error::OK(),
                json::v1::load_typed_config<::rpc::tcp_blocking_stream::endpoint>(
                    tcp_blocking_options_schema(), tcp_blocking_default_options(), client_options)};
        }
        catch (const std::exception&)
        {
            return {rpc::error::INVALID_DATA(), {}};
        }
    }

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service)
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto executor = service && service->get_executor() ? service->get_executor()
                                                           : rpc::connection_factory::make_default_executor();
        if (!executor)
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

        coro::net::tcp::client client(executor, coro::net::socket_address{endpoint.host, endpoint.port});
        auto status = CO_AWAIT client.connect(std::chrono::milliseconds(endpoint.connect_timeout));
        if (status != coro::net::connect_status::connected)
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

        CO_RETURN rpc::connection_factory::stream_result{rpc::error::OK(),
            std::make_shared<::streaming::blocking::tcp::stream>(std::move(client), std::move(executor))};
#else
        const int fd = connect_socket_blocking(endpoint, std::chrono::milliseconds(endpoint.connect_timeout));
        if (fd < 0)
            return rpc::connection_factory::stream_result{rpc::error::TRANSPORT_ERROR(), {}};
        return rpc::connection_factory::stream_result{rpc::error::OK(),
            std::make_shared<::streaming::blocking::tcp::stream>(::streaming::blocking::tcp::socket(fd))};
#endif
    }

    std::shared_ptr<::streaming::stream_acceptor> make_acceptor(const ::rpc::tcp_blocking_stream::endpoint& endpoint)
    {
        return std::make_shared<::streaming::blocking::tcp::acceptor>(endpoint);
    }

    CORO_TASK(rpc::connection_factory::stream_accept_result)
    accept_stream(
        rpc::connection_factory::stream_callback callback,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN rpc::connection_factory::accept_streams(
            make_acceptor(endpoint),
            std::move(callback),
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service),
            {},
            endpoint.port);
    }
} // namespace rpc::tcp_blocking
