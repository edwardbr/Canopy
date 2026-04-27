/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>

#  include <rpc/rpc.h>
#  include <streaming/stream.h>
#  include <transports/libcoro_spsc_dynamic_dll/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::libcoro_spsc_dynamic_dll
{
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> canopy_libcoro_spsc_dll_init(
        const std::string& name,
        std::shared_ptr<rpc::root_service> service,
        std::shared_ptr<streaming::stream> stream);

    // a factory function that creates a stream_transport and redirects it to an app specific function
    template<
        class PARENT_INTERFACE,
        class CHILD_INTERFACE>
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> create_acceptor(
        const std::string& name,
        const std::shared_ptr<rpc::root_service>& service,
        std::shared_ptr<streaming::stream> stream,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>,
            std::shared_ptr<rpc::service>)> factory)
    {
        auto handler = rpc::connection_handler(
            [name, factory = std::move(factory)](
                rpc::connection_settings input,
                std::shared_ptr<rpc::service> svc,
                std::shared_ptr<rpc::transport> transport) -> CORO_TASK(rpc::connection_handler_result)
            {
                auto child_factory = std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
                    rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::child_service>)>(
                    [factory](rpc::shared_ptr<PARENT_INTERFACE> remote, std::shared_ptr<rpc::child_service> child_service)
                        -> CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)
                    {
                        CO_RETURN CO_AWAIT factory(
                            std::move(remote), std::static_pointer_cast<rpc::service>(std::move(child_service)));
                    });

                auto result = CO_AWAIT rpc::child_service::create_child_zone<PARENT_INTERFACE, CHILD_INTERFACE>(
                    name.c_str(), std::move(transport), std::move(input), std::move(child_factory), svc->get_scheduler());
                CO_RETURN rpc::connection_handler_result{result.error_code, std::move(result.descriptor)};
            });

        CO_RETURN rpc::stream_transport::make_server(name, service, std::move(stream), std::move(handler));
    }

} // namespace rpc::libcoro_spsc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
