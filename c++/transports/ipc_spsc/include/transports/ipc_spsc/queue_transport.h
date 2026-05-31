/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <rpc/rpc.h>
#  include <transports/ipc_spsc/dll_abi.h>
#  include <transports/streaming/transport.h>

namespace rpc::ipc_spsc
{
    std::shared_ptr<rpc::stream_transport::transport> make_client(
        std::string name,
        const std::shared_ptr<rpc::service>& service,
        queue_pair* queues);

} // namespace rpc::ipc_spsc

#endif // CANOPY_BUILD_COROUTINE
