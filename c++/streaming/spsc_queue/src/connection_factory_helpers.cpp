/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/spsc_queue.h>

#ifdef CANOPY_BUILD_COROUTINE

#  include <exception>
#  include <utility>

#  include <json/config_loader.h>
#  include <json/convert.h>
#  include <spsc_queue_stream/spsc_queue_stream_config_schema.h>

namespace rpc::spsc_queue
{
    namespace
    {
        CORO_TASK(rpc::connection_factory::stream_result)
        make_stream(
            const queue_pair& queues,
            bool connect_side,
            std::shared_ptr<rpc::service> service)
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
    } // namespace

    const json::v1::object& spsc_queue_default_options()
    {
        static const json::v1::object options = []
        {
            const ::rpc::spsc_queue_stream::stream_settings defaults;
            using json::v1::convert::to_json_object;
            return to_json_object(defaults);
        }();
        return options;
    }

    const json::v1::object& spsc_queue_options_schema()
    {
        static const json::v1::object schema
            = json::v1::parse(::rpc::spsc_queue_stream::stream_settings::get_schema(rpc::encoding::yas_json));
        return schema;
    }

    materialise_spsc_queue_options_result materialise_spsc_queue_options(const json::v1::object& client_options)
    {
        try
        {
            return {rpc::error::OK(),
                json::v1::load_typed_config<::rpc::spsc_queue_stream::stream_settings>(
                    spsc_queue_options_schema(), spsc_queue_default_options(), client_options)};
        }
        catch (const std::exception&)
        {
            return {rpc::error::INVALID_DATA(), {}};
        }
    }

    queue_pair queue_pair::create()
    {
        return {std::make_shared<::streaming::spsc_queue::queue_type>(),
            std::make_shared<::streaming::spsc_queue::queue_type>()};
    }

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN CO_AWAIT make_stream(queues, true, std::move(service));
    }

    CORO_TASK(rpc::connection_factory::stream_result)
    accept_stream(
        const queue_pair& queues,
        std::shared_ptr<rpc::service> service)
    {
        CO_RETURN CO_AWAIT make_stream(queues, false, std::move(service));
    }
} // namespace rpc::spsc_queue

#endif
