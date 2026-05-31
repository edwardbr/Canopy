/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/sgx_coroutine/host/factory.h>

#ifdef CANOPY_BUILD_COROUTINE

#  include <utility>

#  include <io_uring/host_io_uring.h>
#  include <json/convert.h>
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

        auto transport_name = rpc::transport_creation::configured_name(settings.name, "sgx_coroutine_transport");
        auto proxy_name = rpc::transport_creation::configured_name(
            settings.service_proxy_name, rpc::transport_creation::configured_name(settings.name, "sgx_coroutine_child"));

        auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
            transport_name, resolved_service, settings.enclave_path);
        if (settings.enclave)
        {
            using json::v1::convert::to_json_object;
            auto runtime_error = transport->set_enclave_runtime_settings(to_json_object(settings.enclave.value()));
            if (runtime_error != rpc::error::OK())
                return {runtime_error, {}, {}, {}};
            transport->set_enclave_io_uring_options(
                rpc::io_uring::host_controller_options_from_enclave_host_options(settings.enclave.value().io_uring));
        }

        transport->set_enclave_worker_thread_count(settings.worker_thread_count);
        transport->set_use_sidecar(settings.use_sidecar);
        transport->set_peer_to_peer_shared_memory_file(settings.peer_to_peer_shared_memory_file);
        transport->set_sidecar_executable_path(settings.sidecar_executable_path);
        auto startup_error = transport->set_enclave_startup_applications(settings.startup_applications);
        if (startup_error != rpc::error::OK())
            return {startup_error, {}, {}, {}};

        return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
    }
} // namespace rpc::sgx_coroutine_transport::host

#endif
