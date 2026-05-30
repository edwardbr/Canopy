/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <utility>

#include <json/config.h>
#include <connection_factory/stream_rpc.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <spsc_queue_stream/spsc_queue_stream_config.h>
#  include <streaming/spsc_queue/stream.h>

namespace rpc::spsc_queue
{
    const json::v1::object& spsc_queue_default_options();

    struct materialise_spsc_queue_options_result
    {
        int error_code{rpc::error::OK()};
        ::rpc::spsc_queue_stream::stream_settings options;
    };

    const json::v1::object& spsc_queue_options_schema();

    materialise_spsc_queue_options_result materialise_spsc_queue_options(const json::v1::object& client_options);

    struct queue_pair
    {
        std::shared_ptr<::streaming::spsc_queue::queue_type> connect_to_accept;
        std::shared_ptr<::streaming::spsc_queue::queue_type> accept_to_connect;

        static queue_pair create();
    };

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {});

    CORO_TASK(rpc::connection_factory::stream_result)
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
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(settings, std::move(service), "spsc_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(queues, resolved_service);
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
        const queue_pair& queues,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT connect_rpc<In, Out>(
            std::move(input_interface),
            queues,
            rpc::connection_factory::make_stream_rpc_settings(settings),
            std::move(service));
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
            std::move(input_interface), queues, rpc::connection_factory::make_stream_rpc_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(settings, std::move(service), "spsc_rpc_accept");
        if (!resolved_service)
            CO_RETURN rpc::connection_factory::rpc_accept_result{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT accept_stream(queues, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::rpc_accept_result{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_stream<Remote, Local>(
            std::move(stream.stream), std::move(factory), settings, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory), queues, rpc::connection_factory::make_stream_rpc_settings(settings), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::connection_factory::rpc_factory<
            Remote,
            Local> factory,
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            std::move(factory), queues, rpc::connection_factory::make_stream_rpc_settings(), std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const queue_pair& queues,
        const rpc::connection_factory::stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            queues,
            settings,
            std::move(service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            queues,
            rpc::connection_factory::make_stream_rpc_settings(),
            std::move(service));
    }

} // namespace rpc::spsc_queue
#endif
