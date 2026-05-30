/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <utility>

#include <transports/streaming/factory.h>
#include <streaming/tcp_blocking/acceptor.h>

namespace rpc::tcp_blocking
{
    // Typed factories for the blocking TCP stream implementation. JSON
    // materialisation is intentionally not part of this API; config-driven
    // construction is layered on top by configuration adapters.

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {});

    std::shared_ptr<::streaming::stream_acceptor> make_acceptor(const ::rpc::tcp_blocking_stream::endpoint& endpoint);

    CORO_TASK(rpc::stream_transport::stream_accept_result)
    accept_stream(
        rpc::stream_transport::stream_callback callback,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {});

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service
            = rpc::stream_transport::ensure_service(settings, std::move(service), "tcp_blocking_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(endpoint, resolved_service);
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            endpoint,
            rpc::stream_transport::make_connection_settings(settings),
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
            std::move(input_interface), endpoint, rpc::stream_transport::make_connection_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT rpc::stream_transport::accept_rpc_listener<Remote, Local>(
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
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            endpoint,
            rpc::stream_transport::make_connection_settings(settings),
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory),
            endpoint,
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
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            endpoint,
            settings,
            std::move(service),
            std::move(observe_transport));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::listener_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const ::rpc::tcp_blocking_stream::endpoint& endpoint,
        std::shared_ptr<rpc::service> service = {},
        rpc::stream_transport::rpc_transport_observer observe_transport = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            endpoint,
            rpc::stream_transport::make_connection_settings(),
            std::move(service),
            std::move(observe_transport));
    }
} // namespace rpc::tcp_blocking
