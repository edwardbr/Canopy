/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>

#  include <rpc/rpc.h>
#  include <streaming/stream.h>
#  include <transports/streaming/transport.h>

namespace rpc::ipc_transport::ipc_child_process
{
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
} // namespace rpc::ipc_transport::ipc_child_process

#endif // CANOPY_BUILD_COROUTINE
