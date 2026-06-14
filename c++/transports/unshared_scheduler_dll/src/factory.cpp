/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/unshared_scheduler_dll/factory.h>

#ifdef CANOPY_BUILD_COROUTINE

#  include <utility>

#  include <transports/unshared_scheduler_dll/transport.h>

namespace rpc::unshared_scheduler_dll
{
    rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        if (settings.dynamic_library_path.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto resolved_service = rpc::transport_creation::ensure_service(
            std::move(service), settings.encoding, "unshared_scheduler_dll_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto transport = std::make_shared<child_transport>(
            rpc::transport_creation::configured_name(settings.name, "unshared_scheduler_dll"),
            resolved_service,
            settings.dynamic_library_path,
            settings.module_settings ? settings.module_settings.value() : json::v1::object{json::v1::map{}},
            settings.startup_applications);
        return {rpc::error::OK(),
            std::move(resolved_service),
            std::move(transport),
            rpc::transport_creation::configured_name(
                settings.service_proxy_name,
                rpc::transport_creation::configured_name(settings.name, "unshared_scheduler_dll_child"))};
    }
} // namespace rpc::unshared_scheduler_dll

#endif // CANOPY_BUILD_COROUTINE
