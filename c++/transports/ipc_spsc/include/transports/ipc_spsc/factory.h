/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <memory>
#  include <utility>

#  include <ipc_spsc/config.h>
#  include <transports/factory.h>

namespace rpc::ipc_spsc
{
    [[nodiscard]] rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service = {});

    template<
        class In,
        class Out>
    CORO_TASK(rpc::service_connect_result<Out>)
    connect_rpc(
        transport_settings settings,
        rpc::shared_ptr<In> input_interface,
        std::shared_ptr<rpc::service> service = {})
    {
        CO_RETURN CO_AWAIT rpc::transport_creation::connect_rpc<In, Out>(
            std::move(input_interface), connect_transport(settings, std::move(service)));
    }
} // namespace rpc::ipc_spsc

#endif // CANOPY_BUILD_COROUTINE
