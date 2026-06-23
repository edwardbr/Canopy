/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/tcp_coroutine/factory.h>

#include <arpa/inet.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <utility>

#include <streaming/tcp_coroutine/connector.h>

namespace rpc::tcp_coroutine
{
    namespace
    {
        struct endpoint_address
        {
            int error_code{rpc::error::OK()};
            std::array<uint8_t, 4> ipv4{};
            std::array<uint8_t, 16> ipv6{};
        };

        struct tcp_coroutine_runtime_options
        {
            rpc::stream_transport::connection_settings factory_settings;
            rpc::io_uring::host_controller::options controller_options;
            ::streaming::coroutine::tcp::stream::options stream_options;
            uint16_t port{0};
            std::chrono::milliseconds connect_timeout{5000};
        };

        endpoint_address parse_endpoint_address(const ::rpc::tcp_coroutine_stream::endpoint& options) noexcept
        {
            endpoint_address result;
            if (options.ipv6)
            {
                if (::inet_pton(AF_INET6, options.host.c_str(), result.ipv6.data()) != 1)
                    result.error_code = rpc::error::INVALID_DATA();
                return result;
            }

            if (::inet_pton(AF_INET, options.host.c_str(), result.ipv4.data()) != 1)
                result.error_code = rpc::error::INVALID_DATA();
            return result;
        }

        tcp_coroutine_runtime_options make_tcp_coroutine_runtime_options(
            ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
            rpc::stream_transport::connection_settings factory_settings = rpc::stream_transport::make_connection_settings())
        {
            tcp_coroutine_runtime_options result;
            result.factory_settings = std::move(factory_settings);
            result.controller_options = tcp_coroutine_options.controller;
            result.stream_options = tcp_coroutine_options.stream;
            result.port = tcp_coroutine_options.port;
            result.connect_timeout = std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(tcp_coroutine_options.connect_timeout)};
            return result;
        }

        CORO_TASK(int)
        listen_endpoint_port(
            const std::shared_ptr<::streaming::coroutine::tcp::acceptor>& acceptor,
            const ::rpc::tcp_coroutine_stream::endpoint& options,
            const endpoint_address& address,
            uint16_t port,
            uint32_t backlog)
        {
            if (!acceptor)
                CO_RETURN rpc::error::RESOURCE_CLOSED();

            if (options.ipv6)
                CO_RETURN CO_AWAIT acceptor->listen_ipv6(address.ipv6, port, backlog);

            CO_RETURN CO_AWAIT acceptor->listen_ipv4(address.ipv4, port, backlog);
        }
    } // namespace

    runtime make_runtime(
        rpc::io_uring::host_controller::options controller_options,
        std::shared_ptr<rpc::service> service)
    {
        const bool borrowed_scheduler = service && service->get_executor();
        auto scheduler = borrowed_scheduler ? service->get_executor() : rpc::make_executor();
        if (!scheduler)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}, {}};

        if (borrowed_scheduler)
        {
            std::shared_ptr<rpc::io_uring::linux_io_uring_handle> handle;
            auto error_code = rpc::io_uring::linux_io_uring_handle::create(handle, controller_options, scheduler);
            if (error_code != rpc::error::OK())
                return {error_code, {}, {}, {}};

            auto controller = std::make_shared<rpc::io_uring::controller>(
                std::move(handle), scheduler.get(), rpc::io_uring::default_controller_options());
            return {rpc::error::OK(), {}, std::move(scheduler), std::move(controller)};
        }

        std::shared_ptr<rpc::io_uring::io_uring_scheduler> owner;
        auto error_code = rpc::io_uring::create_scheduler(owner, controller_options, scheduler);
        if (error_code != rpc::error::OK())
            return {error_code, {}, {}, {}};

        auto controller = owner ? owner->get_controller() : nullptr;
        if (!controller)
            return {rpc::error::TRANSPORT_ERROR(), {}, {}, {}};

        return {rpc::error::OK(), std::move(owner), std::move(scheduler), std::move(controller)};
    }

    CORO_TASK(listen_result)
    listen_loopback(
        const std::shared_ptr<::streaming::coroutine::tcp::acceptor>& acceptor,
        loopback_listen_options listen_options)
    {
        if (listen_options.port != 0)
        {
            const auto listen_error = CO_AWAIT acceptor->listen_loopback(listen_options.port);
            CO_RETURN listen_result{listen_error, listen_options.port};
        }

        if (listen_options.port_range.first_port >= listen_options.port_range.last_port)
            CO_RETURN listen_result{rpc::error::INVALID_DATA(), 0};

        int listen_error = rpc::error::OK();
        for (uint16_t candidate = listen_options.port_range.first_port; candidate < listen_options.port_range.last_port;
            ++candidate)
        {
            listen_error = CO_AWAIT acceptor->listen_loopback(candidate);
            if (listen_error == rpc::error::OK())
                CO_RETURN listen_result{rpc::error::OK(), candidate};
        }

        CO_RETURN listen_result{listen_error, 0};
    }

    int validate_connect_endpoint(const ::rpc::tcp_coroutine_stream::endpoint& options) noexcept
    {
        if (options.port == 0)
            return rpc::error::INVALID_DATA();

        const auto address = parse_endpoint_address(options);
        if (address.error_code != rpc::error::OK())
            return address.error_code;

        return rpc::error::OK();
    }

    CORO_TASK(listen_result)
    listen_endpoint(
        const std::shared_ptr<::streaming::coroutine::tcp::acceptor>& acceptor,
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        uint32_t backlog)
    {
        const auto address = parse_endpoint_address(options);
        if (address.error_code != rpc::error::OK())
            CO_RETURN listen_result{address.error_code, 0};

        if (options.port != 0)
        {
            const auto listen_error = CO_AWAIT listen_endpoint_port(acceptor, options, address, options.port, backlog);
            const auto resolved_port = listen_error == rpc::error::OK() ? options.port : uint16_t{0};
            CO_RETURN listen_result{listen_error, resolved_port};
        }

        if (options.first_port >= options.last_port)
            CO_RETURN listen_result{rpc::error::INVALID_DATA(), 0};

        int listen_error = rpc::error::OK();
        for (uint16_t candidate = options.first_port; candidate < options.last_port; ++candidate)
        {
            listen_error = CO_AWAIT listen_endpoint_port(acceptor, options, address, candidate, backlog);
            if (listen_error == rpc::error::OK())
                CO_RETURN listen_result{rpc::error::OK(), candidate};
        }

        CO_RETURN listen_result{listen_error, 0};
    }

    CORO_TASK(acceptor_result)
    listen_acceptor(
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options,
        ::streaming::coroutine::tcp::stream::options stream_options,
        std::shared_ptr<rpc::service> service)
    {
        auto resolved_service = rpc::stream_transport::ensure_service(
            rpc::stream_transport::make_connection_settings(), std::move(service), "tcp_coroutine_stream_accept");
        if (!resolved_service)
            CO_RETURN acceptor_result{rpc::error::INVALID_DATA(), {}, {}, {}, 0};

        auto runtime = make_runtime(controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN acceptor_result{runtime.error_code, {}, {}, {}, 0};

        auto acceptor = std::make_shared<::streaming::coroutine::tcp::acceptor>(runtime.controller, stream_options);
        const auto listen_result = CO_AWAIT listen_loopback(acceptor, listen_options);
        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN acceptor_result{listen_result.error_code, {}, {}, {}, 0};
        if (listen_result.port == 0)
            CO_RETURN acceptor_result{rpc::error::TRANSPORT_ERROR(), {}, {}, {}, 0};

        CO_RETURN acceptor_result{rpc::error::OK(),
            std::move(resolved_service),
            std::move(acceptor),
            std::move(runtime.scheduler_owner),
            listen_result.port};
    }

    CORO_TASK(acceptor_result)
    listen_acceptor(
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        const rpc::stream_transport::connection_settings& factory_settings,
        std::shared_ptr<rpc::service> service)
    {
        auto runtime_options = make_tcp_coroutine_runtime_options(options, factory_settings);
        auto resolved_service = rpc::stream_transport::ensure_service(
            runtime_options.factory_settings, std::move(service), "tcp_coroutine_stream_accept");
        if (!resolved_service)
            CO_RETURN acceptor_result{rpc::error::INVALID_DATA(), {}, {}, {}, 0};

        auto runtime = make_runtime(runtime_options.controller_options, resolved_service);
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN acceptor_result{runtime.error_code, {}, {}, {}, 0};

        auto acceptor
            = std::make_shared<::streaming::coroutine::tcp::acceptor>(runtime.controller, runtime_options.stream_options);
        const auto listen_result = CO_AWAIT listen_endpoint(acceptor, options);
        if (listen_result.error_code != rpc::error::OK())
            CO_RETURN acceptor_result{listen_result.error_code, {}, {}, {}, 0};
        if (listen_result.port == 0)
            CO_RETURN acceptor_result{rpc::error::TRANSPORT_ERROR(), {}, {}, {}, 0};

        CO_RETURN acceptor_result{rpc::error::OK(),
            std::move(resolved_service),
            std::move(acceptor),
            std::move(runtime.scheduler_owner),
            listen_result.port};
    }

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options,
        ::streaming::coroutine::tcp::stream::options stream_options,
        std::shared_ptr<rpc::service> service)
    {
        if (port == 0)
            CO_RETURN rpc::stream_transport::stream_result{rpc::error::INVALID_DATA(), {}};

        auto runtime = make_runtime(controller_options, std::move(service));
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_result{runtime.error_code, {}};

        auto result = CO_AWAIT ::streaming::coroutine::tcp::connect_loopback(
            runtime.controller, port, stream_options, std::chrono::milliseconds{5000}, runtime.scheduler);
        if (result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_result{result.error_code, {}, result.native_result};

        CO_RETURN rpc::stream_transport::stream_result{rpc::error::OK(),
            rpc::stream_transport::keep_owner(std::move(result.connection), std::move(runtime.scheduler_owner)),
            result.native_result};
    }

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        std::shared_ptr<rpc::service> service)
    {
        const auto validation_error = validate_connect_endpoint(options);
        if (validation_error != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_result{validation_error, {}};

        const auto runtime_options = make_tcp_coroutine_runtime_options(options);
        auto runtime = make_runtime(runtime_options.controller_options, std::move(service));
        if (runtime.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_result{runtime.error_code, {}};

        const auto address = parse_endpoint_address(options);
        ::streaming::coroutine::tcp::stream_result result;
        if (options.ipv6)
        {
            result = CO_AWAIT ::streaming::coroutine::tcp::connect_ipv6(
                runtime.controller,
                address.ipv6,
                runtime_options.port,
                runtime_options.stream_options,
                runtime_options.connect_timeout,
                runtime.scheduler);
        }
        else
        {
            result = CO_AWAIT ::streaming::coroutine::tcp::connect_ipv4(
                runtime.controller,
                address.ipv4,
                runtime_options.port,
                runtime_options.stream_options,
                runtime_options.connect_timeout,
                runtime.scheduler);
        }
        if (result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_result{result.error_code, {}, result.native_result};

        CO_RETURN rpc::stream_transport::stream_result{rpc::error::OK(),
            rpc::stream_transport::keep_owner(std::move(result.connection), std::move(runtime.scheduler_owner)),
            result.native_result};
    }

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options,
        ::streaming::coroutine::tcp::stream::options stream_options,
        std::shared_ptr<rpc::service> service)
    {
        auto acceptor_result
            = CO_AWAIT listen_acceptor(listen_options, controller_options, stream_options, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_accept_result{acceptor_result.error_code, {}};

        CO_RETURN rpc::stream_transport::accept_streams(
            std::move(acceptor_result.acceptor),
            std::move(callback),
            rpc::stream_transport::make_connection_settings(),
            std::move(acceptor_result.service),
            std::move(acceptor_result.owner),
            acceptor_result.port);
    }

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        const rpc::stream_transport::connection_settings& factory_settings,
        std::shared_ptr<rpc::service> service)
    {
        auto rpc_settings = factory_settings;
        auto acceptor_result = CO_AWAIT listen_acceptor(options, rpc_settings, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::stream_accept_result{acceptor_result.error_code, {}};

        CO_RETURN rpc::stream_transport::accept_streams(
            std::move(acceptor_result.acceptor),
            std::move(callback),
            std::move(rpc_settings),
            std::move(acceptor_result.service),
            std::move(acceptor_result.owner),
            acceptor_result.port);
    }
} // namespace rpc::tcp_coroutine
