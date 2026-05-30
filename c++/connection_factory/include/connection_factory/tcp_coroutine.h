/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <io_uring/host_io_uring.h>
#include <connection_factory/tcp_coroutine_options.h>
#include <connection_factory/stream_rpc.h>
#include <streaming/tcp_coroutine/acceptor.h>

namespace rpc::tcp_coroutine
{
    // High-level stream/RPC factories for TCP coroutine streams backed by io_uring. JSON option normalisation
    // lives in connection_factory/tcp_coroutine_options.h so direct controller/stream
    // code can use typed io_uring options without pulling factory mechanics into
    // the same header.

    struct runtime
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::io_uring::io_uring_scheduler> scheduler_owner;
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
        const std::shared_ptr<::streaming::coroutine::tcp::acceptor>& acceptor,
        loopback_listen_options listen_options);

    CORO_TASK(listen_result)
    listen_endpoint(
        const std::shared_ptr<::streaming::coroutine::tcp::acceptor>& acceptor,
        const ::rpc::tcp_coroutine_stream::endpoint& options,
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
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        const rpc::connection_factory::stream_rpc_connection_settings& factory_settings
        = rpc::connection_factory::make_stream_rpc_settings(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        uint16_t port,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::connection_factory::stream_accept_result)
    accept_stream(
        rpc::connection_factory::stream_callback callback,
        loopback_listen_options listen_options = {},
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::connection_factory::stream_accept_result)
    accept_stream(
        rpc::connection_factory::stream_callback callback,
        const ::rpc::tcp_coroutine_stream::endpoint& options,
        const rpc::connection_factory::stream_rpc_connection_settings& factory_settings
        = rpc::connection_factory::make_stream_rpc_settings(),
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
        auto resolved_service = rpc::connection_factory::ensure_service(
            rpc::connection_factory::make_stream_rpc_settings(), std::move(service), "tcp_coroutine_rpc_client");
        auto stream = CO_AWAIT connect_stream(port, controller_options, stream_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface),
            std::move(stream.stream),
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto runtime_options = detail::make_tcp_coroutine_runtime_options(tcp_coroutine_options, settings);
        auto resolved_service = rpc::connection_factory::ensure_service(
            runtime_options.factory_settings, std::move(service), "tcp_coroutine_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        auto stream = CO_AWAIT connect_stream(
            runtime_options.port, runtime_options.controller_options, runtime_options.stream_options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface),
            std::move(stream.stream),
            runtime_options.factory_settings,
            std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            tcp_coroutine_options,
            rpc::connection_factory::make_stream_rpc_settings(settings),
            std::move(service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            tcp_coroutine_options,
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        auto acceptor_result
            = CO_AWAIT listen_acceptor(listen_options, controller_options, stream_options, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{acceptor_result.error_code, {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
            std::move(acceptor_result.acceptor),
            std::move(factory),
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(acceptor_result.service),
            std::move(acceptor_result.owner),
            acceptor_result.port,
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        auto rpc_settings = settings;
        auto acceptor_result = CO_AWAIT listen_acceptor(tcp_coroutine_options, rpc_settings, std::move(service));
        if (acceptor_result.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::listener_result{acceptor_result.error_code, {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
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
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            tcp_coroutine_options,
            rpc::connection_factory::make_stream_rpc_settings(settings),
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            tcp_coroutine_options,
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        loopback_listen_options listen_options,
        rpc::io_uring::host_controller::options controller_options = {},
        ::streaming::coroutine::tcp::stream::options stream_options = ::streaming::coroutine::tcp::default_stream_options(),
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            listen_options,
            controller_options,
            stream_options,
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            tcp_coroutine_options,
            settings,
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const ::rpc::tcp_coroutine_stream::endpoint& tcp_coroutine_options,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            tcp_coroutine_options,
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service),
            std::move(observe_transport));
    }

} // namespace rpc::tcp_coroutine
