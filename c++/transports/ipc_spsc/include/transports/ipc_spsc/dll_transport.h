/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>
#  include <string>
#  include <utility>

#  include <rpc/rpc.h>
#  include <streaming/stream.h>
#  include <transports/ipc_spsc/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::ipc_spsc
{
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> canopy_ipc_spsc_dll_init(
        std::string name,
        std::shared_ptr<rpc::root_service> service,
        std::shared_ptr<streaming::stream> stream);

    template<class PARENT_INTERFACE, class CHILD_INTERFACE> class child_factory_adapter
    {
    public:
        explicit child_factory_adapter(
            std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
                rpc::shared_ptr<PARENT_INTERFACE>,
                std::shared_ptr<rpc::service>)> factory)
            : factory_(std::move(factory))
        {
        }

        CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)
        operator()(
            rpc::shared_ptr<PARENT_INTERFACE> remote,
            std::shared_ptr<rpc::child_service> child_service) const
        {
            CO_RETURN CO_AWAIT factory_(
                std::move(remote), std::static_pointer_cast<rpc::service>(std::move(child_service)));
        }

    private:
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::service>)>
            factory_;
    };

    template<class PARENT_INTERFACE, class CHILD_INTERFACE> class child_zone_connection_handler
    {
    public:
        child_zone_connection_handler(
            std::string name,
            std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
                rpc::shared_ptr<PARENT_INTERFACE>,
                std::shared_ptr<rpc::service>)> factory)
            : name_(std::move(name))
            , factory_(std::move(factory))
        {
        }

        CORO_TASK(rpc::connection_handler_result)
        operator()(
            rpc::connection_settings input,
            std::shared_ptr<rpc::service> svc,
            std::shared_ptr<rpc::transport> transport) const
        {
            auto child_factory = std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
                rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::child_service>)>(
                child_factory_adapter<PARENT_INTERFACE, CHILD_INTERFACE>{factory_});

            auto result = CO_AWAIT rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
                name_, std::move(transport), std::move(input), std::move(child_factory), svc->get_executor());
            CO_RETURN rpc::connection_handler_result{result.error_code, std::move(result.descriptor)};
        }

    private:
        std::string name_;
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::service>)>
            factory_;
    };

    // a factory function that creates a stream_transport and redirects it to an app specific function
    template<
        class PARENT_INTERFACE,
        class CHILD_INTERFACE>
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> create_acceptor(
        std::string name,
        std::shared_ptr<rpc::root_service> service,
        std::shared_ptr<streaming::stream> stream,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>,
            std::shared_ptr<rpc::service>)> factory)
    {
        auto transport_name = name;
        auto handler = rpc::connection_handler(
            child_zone_connection_handler<PARENT_INTERFACE, CHILD_INTERFACE>{std::move(name), std::move(factory)});

        CO_RETURN rpc::stream_transport::create(
            std::move(transport_name), std::move(service), std::move(stream), std::move(handler));
    }

} // namespace rpc::ipc_spsc

#endif // CANOPY_BUILD_COROUTINE
