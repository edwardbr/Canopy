/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/host/factory.h>

#ifdef CANOPY_BUILD_COROUTINE

#  include <utility>

#  include <transports/sgx_coroutine/host/transport.h>

namespace rpc::sgx_coroutine_transport::host
{
    rpc::transport_creation::connect_result connect_transport(
        const rpc::sgx_coroutine_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        if (settings.enclave_path.empty())
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto resolved_service
            = rpc::transport_creation::ensure_service(std::move(service), settings.encoding, "sgx_coroutine_rpc_client");
        if (!resolved_service)
            return {rpc::error::INVALID_DATA(), {}, {}, {}};

        auto startup_error = rpc::sgx_coroutine_transport::host::transport::validate_startup_settings(settings);
        if (startup_error != rpc::error::OK())
            return {startup_error, {}, {}, {}};

        auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
            rpc::transport_creation::configured_name(settings.name, "sgx_coroutine_transport"), resolved_service, settings);

        return {rpc::error::OK(),
            std::move(resolved_service),
            std::move(transport),
            rpc::transport_creation::configured_name(
                settings.service_proxy_name,
                rpc::transport_creation::configured_name(settings.name, "sgx_coroutine_child"))};
    }
} // namespace rpc::sgx_coroutine_transport::host

#endif
