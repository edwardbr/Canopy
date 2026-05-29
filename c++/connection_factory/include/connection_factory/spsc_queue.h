/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <utility>

#include <connection_factory/stream_rpc.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <streaming/spsc_queue/stream.h>

namespace rpc::spsc_queue
{
    struct queue_pair
    {
        std::shared_ptr<::streaming::spsc_queue::queue_type> connect_to_accept;
        std::shared_ptr<::streaming::spsc_queue::queue_type> accept_to_connect;

        static queue_pair create()
        {
            return {std::make_shared<::streaming::spsc_queue::queue_type>(),
                std::make_shared<::streaming::spsc_queue::queue_type>()};
        }
    };

    namespace detail
    {
        inline CORO_TASK(rpc::connection_factory::stream_result) make_stream(
            const queue_pair& queues,
            bool connect_side,
            std::shared_ptr<rpc::service> service = {})
        {
            auto scheduler = service && service->get_executor() ? service->get_executor()
                                                                : rpc::connection_factory::make_default_executor();
            if (!scheduler)
                CO_RETURN rpc::connection_factory::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

            const auto& outbound = connect_side ? queues.connect_to_accept : queues.accept_to_connect;
            const auto& inbound = connect_side ? queues.accept_to_connect : queues.connect_to_accept;
            auto stream = std::make_shared<::streaming::spsc_queue::stream>(outbound, inbound, scheduler);
            std::shared_ptr<void> owner;
            if (!service)
                owner = scheduler;
            CO_RETURN rpc::connection_factory::stream_result{
                rpc::error::OK(), rpc::connection_factory::keep_owner(std::move(stream), std::move(owner))};
        }
    } // namespace detail

    inline CORO_TASK(rpc::connection_factory::stream_result) connect_stream(
        const queue_pair& queues,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        (void)options;
        CO_RETURN CO_AWAIT detail::make_stream(queues, true, std::move(service));
    }

    inline CORO_TASK(rpc::connection_factory::stream_result) accept_stream(
        const queue_pair& queues,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        (void)options;
        CO_RETURN CO_AWAIT detail::make_stream(queues, false, std::move(service));
    }

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        rpc::shared_ptr<In> input_interface,
        const queue_pair& queues,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(options, std::move(service), "spsc_rpc_client");
        if (!resolved_service)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT connect_stream(queues, options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::connect_rpc_stream<In, Out>(
            std::move(input_interface), std::move(stream.stream), options, std::move(resolved_service));
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
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        auto resolved_service = rpc::connection_factory::ensure_service(options, std::move(service), "spsc_rpc_accept");
        if (!resolved_service)
            CO_RETURN rpc::connection_factory::rpc_accept_result{rpc::error::INVALID_DATA(), {}};
        auto stream = CO_AWAIT accept_stream(queues, options, resolved_service);
        if (stream.error_code != rpc::error::OK())
            CO_RETURN rpc::connection_factory::rpc_accept_result{stream.error_code, {}};
        CO_RETURN CO_AWAIT rpc::connection_factory::accept_rpc_stream<Remote, Local>(
            std::move(stream.stream), std::move(factory), options, std::move(resolved_service));
    }

    template<
        class Remote,
        class Local>
    CORO_TASK(rpc::connection_factory::rpc_accept_result)
    accept_rpc(
        rpc::shared_ptr<Local> local_interface,
        const queue_pair& queues,
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT accept_rpc<Remote, Local>(
            rpc::connection_factory::fixed_factory<Remote, Local>(std::move(local_interface)),
            queues,
            options,
            std::move(service));
    }

} // namespace rpc::spsc_queue
#endif
