/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <io_uring/host_io_uring.h>
#include <transports/streaming/factory.h>
#include <streaming/tcp_coroutine/acceptor.h>
#include <streaming/tcp_coroutine/stream.h>
#include <tcp_coroutine_stream/tcp_coroutine_stream_config.h>

namespace rpc::tcp_coroutine
{
    // Typed factories for TCP coroutine streams backed by io_uring. JSON
    // materialisation is intentionally not part of this API; config-driven
    // construction is layered on top by configuration adapters.

    struct loopback_port_range
    {
        uint16_t first_port{26000};
        uint16_t last_port{26064};
    };

    struct loopback_listen_options
    {
        // A non-zero port asks for one exact loopback port. Zero means scan the
        // range below and use the first available port.
        uint16_t port{0};
        loopback_port_range port_range{};
    };

    int validate_connect_endpoint(const ::rpc::tcp_coroutine_stream::endpoint& options) noexcept;

    struct runtime
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
        rpc::executor_ptr scheduler;
        std::shared_ptr<rpc::io_uring::controller> controller;
    };

    runtime make_runtime(
        rpc::io_uring::host_controller::options controller_options = {},
        std::shared_ptr<rpc::service> service = {});

    struct listen_result
    {
        int error_code{rpc::error::OK()};
        uint16_t port{0};
    };

    struct acceptor_result
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<::streaming::coroutine::tcp::acceptor> acceptor;
        std::shared_ptr<void> owner;
        uint16_t port{0};
    };

    CORO_TASK(listen_result)
    listen_loopback(
        std::shared_ptr<::streaming::coroutine::tcp::acceptor> acceptor,
        loopback_listen_options listen_options);

    CORO_TASK(listen_result)
    listen_endpoint(
        std::shared_ptr<::streaming::coroutine::tcp::acceptor> acceptor,
        ::rpc::tcp_coroutine_stream::endpoint options,
        uint32_t backlog = 16);

    // Explicit loopback convenience for tests and local demos. Endpoint-based
    // overloads below are the normal TCP surface for real network addresses.
    CORO_TASK(acceptor_result)
    listen_acceptor(
        loopback_listen_options listen_options = {},
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(acceptor_result)
    listen_acceptor(
        ::rpc::tcp_coroutine_stream::endpoint options,
        rpc::stream_transport::connection_settings factory_settings = rpc::stream_transport::make_connection_settings(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        ::rpc::tcp_coroutine_stream::endpoint options,
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        loopback_listen_options listen_options = {},
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        ::rpc::tcp_coroutine_stream::endpoint options,
        rpc::stream_transport::connection_settings factory_settings = rpc::stream_transport::make_connection_settings(),
        std::shared_ptr<rpc::service> service = {});

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::stream_transport::ensure_service(
            rpc::stream_transport::make_connection_settings(), std::move(service), "tcp_coroutine_rpc_client");
        auto stream = CO_AWAIT connect_stream(port, controller_options, stream_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::stream_transport::connect_rpc_stream<In, Out>(
            std::move(input_interface),
            std::move(stream.stream),
            rpc::stream_transport::make_connection_settings(),
            std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        rpc::stream_transport::connection_settings settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service
            = rpc::stream_transport::ensure_service(settings, std::move(service), "tcp_coroutine_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_stream(tcp_coroutine_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        CO_RETURN CO_AWAIT rpc::stream_transport::connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), settings, std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        rpc::stream_transport::transport_settings settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            std::move(tcp_coroutine_options),
            rpc::stream_transport::make_connection_settings(std::move(settings)),
            std::move(service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            std::move(tcp_coroutine_options),
            rpc::stream_transport::make_connection_settings(),
            std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        auto acceptor_result
            = CO_AWAIT listen_acceptor(listen_options, controller_options, stream_options, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::listener_result{acceptor_result.error_code, {}};

        CO_RETURN CO_AWAIT rpc::stream_transport::accept_rpc_listener<Remote, Local>(
            std::move(acceptor_result.acceptor),
            std::move(factory),
            rpc::stream_transport::make_connection_settings(),
            std::move(acceptor_result.service),
            std::move(acceptor_result.owner),
            acceptor_result.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        rpc::stream_transport::connection_settings settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        auto rpc_settings = settings;
        auto acceptor_result = CO_AWAIT listen_acceptor(tcp_coroutine_options, rpc_settings, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::listener_result{acceptor_result.error_code, {}};

        CO_RETURN CO_AWAIT rpc::stream_transport::accept_rpc_listener<Remote, Local>(
            std::move(acceptor_result.acceptor),
            std::move(factory),
            std::move(rpc_settings),
            std::move(acceptor_result.service),
            std::move(acceptor_result.owner),
            acceptor_result.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        rpc::stream_transport::transport_settings settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            std::move(tcp_coroutine_options),
            rpc::stream_transport::make_connection_settings(std::move(settings)),
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            std::move(tcp_coroutine_options),
            rpc::stream_transport::make_connection_settings(),
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            listen_options,
            controller_options,
            stream_options,
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        rpc::stream_transport::connection_settings settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            std::move(tcp_coroutine_options),
            std::move(settings),
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        ::rpc::tcp_coroutine_stream::endpoint tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            std::move(tcp_coroutine_options),
            rpc::stream_transport::make_connection_settings(),
            std::move(service),
            std::move(observe_transport));
    }
} // namespace rpc::tcp_coroutine
