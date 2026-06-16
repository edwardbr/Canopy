/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <utility>

#include <transports/streaming/factory.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <spsc_queue_stream/spsc_queue_stream_config.h>
#  include <streaming/spsc_queue/stream.h>

namespace rpc::spsc_queue
{
    // Typed factories for the SPSC queue stream implementation. JSON
    // materialisation is intentionally not part of this API; config-driven
    // construction is layered on top by configuration adapters.

    struct queue_pair
    {
        std::shared_ptr<::streaming::spsc_queue::queue_type> connect_to_accept;
        std::shared_ptr<::streaming::spsc_queue::queue_type> accept_to_connect;

        static queue_pair create();
    };

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::stream_transport::stream_result)
    accept_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {});

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const queue_pair& queues,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::stream_transport::ensure_service(settings, std::move(service), "spsc_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(queues, resolved_service);
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
        const queue_pair& queues,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface), queues, rpc::stream_transport::make_connection_settings(settings), std::move(service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface), queues, rpc::stream_transport::make_connection_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::rpc_accept_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::stream_transport::ensure_service(settings, std::move(service), "spsc_rpc_accept");
        if (!resolved_service)
            CO_RETURN rpc::stream_transport::rpc_accept_result{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT accept_stream(queues, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::stream_transport::rpc_accept_result{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::stream_transport::accept_rpc_stream<Remote, Local>(
            std::move(stream.stream), std::move(factory), settings, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::rpc_accept_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory), queues, rpc::stream_transport::make_connection_settings(settings), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::rpc_accept_result)
    accept_rpc(
        rpc::stream_transport::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory), queues, rpc::stream_transport::make_connection_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::rpc_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const queue_pair& queues,
        const rpc::stream_transport::connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            queues,
            settings,
            std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::stream_transport::rpc_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::stream_transport::fixed_factory<Remote, Local>(std::move(local_interface)),
            queues,
            rpc::stream_transport::make_connection_settings(),
            std::move(service));
    }
} // namespace rpc::spsc_queue
#endif
