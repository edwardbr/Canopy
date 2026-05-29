/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <json/convert.h>
#include <connection_factory/stream_rpc.h>
#include <streaming/tcp/acceptor.h>
#include <streaming/tcp/stream.h>

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

namespace rpc::tcp
{
    inline const json::v1::object& tcp_default_options()
    {
        static const json::v1::object options = []
        {
            const rpc::connection_factory_config::stream_factory_options defaults;
            using json::v1::convert::to_json_object;
            return to_json_object(defaults);
        }();
        return options;
    }

    // Boundary for sparse JSON configuration: overlay TCP defaults, validate
    // the effective result, and convert to the generated typed options. The
    // factory functions below take typed options directly.
    inline rpc::connection_factory::materialise_options_result materialise_tcp_options(const json::v1::object& client_options)
    {
        return rpc::connection_factory::materialise_options(client_options, tcp_default_options());
    }

#ifndef CANOPY_BUILD_COROUTINE
    inline int connect_socket_blocking(
        const ::streaming::tcp::endpoint& endpoint,
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

    inline CORO_TASK(rpc::connection_factory::stream_result) connect_stream(
        const ::streaming::tcp::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {})
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

        CO_RETURN rpc::connection_factory::stream_result{
            rpc::error::OK(), std::make_shared<::streaming::tcp::stream>(std::move(client), std::move(executor))};
#else
        const int fd = connect_socket_blocking(endpoint, std::chrono::milliseconds(endpoint.connect_timeout));
        if (fd < 0)
            return rpc::connection_factory::stream_result{rpc::error::TRANSPORT_ERROR(), {}};
        return rpc::connection_factory::stream_result{
            rpc::error::OK(), std::make_shared<::streaming::tcp::stream>(::streaming::tcp::socket(fd))};
#endif
    }

    inline CORO_TASK(rpc::connection_factory::stream_result) connect_stream(
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_stream(options.endpoint, std::move(service));
    }

    inline CORO_TASK(rpc::connection_factory::stream_accept_result) accept_stream(
        rpc::connection_factory::stream_callback callback,
        const ::streaming::tcp::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN rpc::connection_factory::accept_streams(
            std::make_shared<::streaming::tcp::acceptor>(endpoint),
            std::move(callback),
            rpc::connection_factory_config::stream_factory_options{},
            std::move(service),
            {},
            endpoint.port);
    }

    inline CORO_TASK(rpc::connection_factory::stream_accept_result) accept_stream(
        rpc::connection_factory::stream_callback callback,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN rpc::connection_factory::accept_streams(
            std::make_shared<::streaming::tcp::acceptor>(options.endpoint),
            std::move(callback),
            options,
            std::move(service),
            {},
            options.endpoint.port);
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(options, std::move(service), "tcp_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), options, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
            std::make_shared<::streaming::tcp::acceptor>(options.endpoint),
            std::move(factory),
            options,
            std::move(service),
            {},
            options.endpoint.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            options,
            std::move(service),
            std::move(observe_transport));
    }

} // namespace rpc::tcp
