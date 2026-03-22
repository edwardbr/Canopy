/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>

#  include <rpc/rpc.h>
#  include <streaming/stream.h>
#  include <transports/libcoro_ipc_dynamic_dll/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::libcoro_ipc_dynamic_dll
{
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> canopy_libcoro_ipc_dll_init(
        const std::string& name, std::shared_ptr<rpc::root_service> service, std::shared_ptr<streaming::stream> stream);

    // a factory function that creates a stream_transport and redirects it to an app specific function
    template<class PARENT_INTERFACE, class CHILD_INTERFACE>
    coro::task<std::shared_ptr<rpc::stream_transport::transport>> create_acceptor(const std::string& name,
        const std::shared_ptr<rpc::root_service>& service,
        std::shared_ptr<streaming::stream> stream,
        std::function<CORO_TASK(rpc::service_connect_result<CHILD_INTERFACE>)(
            rpc::shared_ptr<PARENT_INTERFACE>, std::shared_ptr<rpc::service>)> factory)
    {
        CO_RETURN std::static_pointer_cast<rpc::stream_transport::transport>(
            CO_AWAIT service->template make_acceptor<PARENT_INTERFACE, CHILD_INTERFACE>(
                name, rpc::stream_transport::transport_factory(std::move(stream)), std::move(factory)));
    }

} // namespace rpc::libcoro_ipc_dynamic_dll

#endif // CANOPY_BUILD_COROUTINE
