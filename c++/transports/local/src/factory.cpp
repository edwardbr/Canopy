/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/local/factory.h>

#include <utility>

#include <transports/local/transport.h>

namespace rpc::local_transport
{
    rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        auto resolved_service
            = rpc::transport_creation::ensure_service(std::move(service), settings.encoding, "local_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto transport_name = rpc::transport_creation::configured_name(settings.name, "main child");
        auto proxy_name = rpc::transport_creation::configured_name(
            settings.service_proxy_name, rpc::transport_creation::configured_name(settings.name, "main child"));

        auto transport = std::make_shared<rpc::local::child_transport>(transport_name, resolved_service);
        return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
    }
} // namespace rpc::local_transport
