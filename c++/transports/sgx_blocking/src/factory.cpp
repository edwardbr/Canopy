/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_blocking/factory.h>

#ifndef CANOPY_BUILD_COROUTINE

#  include <utility>

#  include <transports/sgx_blocking/transport.h>

namespace rpc::sgx_blocking_transport
{
    rpc::transport_creation::connect_result connect_transport(
        const transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        if (settings.enclave_path.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto resolved_service
            = rpc::transport_creation::ensure_service(std::move(service), settings.encoding, "sgx_blocking_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto transport_name = rpc::transport_creation::configured_name(settings.name, "sgx_blocking_transport");
        auto proxy_name = rpc::transport_creation::configured_name(
            settings.service_proxy_name, rpc::transport_creation::configured_name(settings.name, "sgx_blocking_child"));

        auto startup_error = rpc::sgx_blocking_transport::enclave_transport::validate_startup_settings(settings);
        if (startup_error != rpc::error::OK())
            return {startup_error, {}, {}, {}};

        auto transport = std::make_shared<rpc::sgx_blocking_transport::enclave_transport>(
            transport_name, resolved_service, settings);

        return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
    }
} // namespace rpc::sgx_blocking_transport

#endif
