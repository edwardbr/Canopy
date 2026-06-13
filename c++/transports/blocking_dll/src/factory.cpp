/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/blocking_dll/factory.h>

#ifndef CANOPY_BUILD_COROUTINE

#  include <utility>

#  include <transports/blocking_dll/transport.h>

namespace rpc::blocking_dll
{
    rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        if (settings.dynamic_library_path.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto resolved_service
            = rpc::transport_creation::ensure_service(std::move(service), settings.encoding, "blocking_dll_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto transport = std::make_shared<child_transport>(
            rpc::transport_creation::configured_name(settings.name, "blocking_dll"),
            resolved_service,
            settings.dynamic_library_path);
        return {rpc::error::OK(),
            std::move(resolved_service),
            std::move(transport),
            rpc::transport_creation::configured_name(
                settings.service_proxy_name, rpc::transport_creation::configured_name(settings.name, "blocking_dll_child"))};
    }
} // namespace rpc::blocking_dll

#endif // !CANOPY_BUILD_COROUTINE
