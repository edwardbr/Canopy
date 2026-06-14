/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/spsc_queue/factory.h>

#ifdef CANOPY_BUILD_COROUTINE

#  include <utility>

namespace rpc::spsc_queue
{
    namespace
    {
        CORO_TASK(rpc::stream_transport::stream_result)
        make_stream(
            const queue_pair& queues,
            bool connect_side,
            std::shared_ptr<rpc::service> service)
        {
            auto scheduler = service && service->get_executor() ? service->get_executor() : rpc::make_executor();
            if (!scheduler)
                CO_RETURN rpc::stream_transport::stream_result{rpc::error::TRANSPORT_ERROR(), {}};

            const auto& outbound = connect_side ? queues.connect_to_accept : queues.accept_to_connect;
            const auto& inbound = connect_side ? queues.accept_to_connect : queues.connect_to_accept;
            auto stream = std::make_shared<::streaming::spsc_queue::stream>(outbound, inbound, scheduler);
            std::shared_ptr<void> owner;
            if (!service)
                owner = scheduler;
            CO_RETURN rpc::stream_transport::stream_result{
                rpc::error::OK(), rpc::stream_transport::keep_owner(std::move(stream), std::move(owner))};
        }
    } // namespace

    queue_pair queue_pair::create()
    {
        return {std::make_shared<::streaming::spsc_queue::queue_type>(),
            std::make_shared<::streaming::spsc_queue::queue_type>()};
    }

    CORO_TASK(rpc::stream_transport::stream_result)
    connect_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN CO_AWAIT make_stream(queues, true, std::move(service));
    }

    CORO_TASK(rpc::stream_transport::stream_result)
    accept_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN CO_AWAIT make_stream(queues, false, std::move(service));
    }
} // namespace rpc::spsc_queue

#endif
