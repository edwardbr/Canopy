/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <utility>

#include <connection_factory/stream_rpc.h>
#include <streaming/tcp_blocking/acceptor.h>

namespace rpc::tcp_blocking
{
    const json::v1::object& tcp_blocking_default_options();

    struct materialise_tcp_blocking_options_result
    {
        int error_code{rpc::error::OK()};
        ::rpc::tcp_blocking_stream::endpoint options;
    };

    const json::v1::object& tcp_blocking_options_schema();

    // Boundary for sparse TCP JSON configuration. Public RPC factories below
    // still take the generated endpoint type directly.
    materialise_tcp_blocking_options_result materialise_tcp_blocking_options(const json::v1::object& client_options);

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {});

    std::shared_ptr<::streaming::stream_acceptor> make_acceptor(const ::rpc::tcp_blocking_stream::endpoint& endpoint);

    CORO_TASK(rpc::connection_factory::stream_accept_result)
    accept_stream(
        rpc::connection_factory::stream_callback callback,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {});

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service
            = rpc::connection_factory::ensure_service(settings, std::move(service), "tcp_blocking_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(endpoint, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), settings, std::move(resolved_service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            endpoint,
            rpc::connection_factory::make_stream_rpc_settings(settings),
            std::move(service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface), endpoint, rpc::connection_factory::make_stream_rpc_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::listener_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_listener<Remote, Local>(
            make_acceptor(endpoint),
            std::move(factory),
            settings,
            std::move(service),
            {},
            endpoint.port,
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            endpoint,
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            endpoint,
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            endpoint,
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {},
        rpc::connection_factory::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            endpoint,
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service),
            std::move(observe_transport));
    }

} // namespace rpc::tcp_blocking
