/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <json/convert.h>
#include <sgx_coroutine_transport/sgx_coroutine_transport_config.h>
#include <sgx_coroutine_transport/sgx_coroutine_transport_config_schema.h>
#include <transports/sgx_coroutine/host/transport.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class sgx_coroutine_transport_component_factory final : public transport_component_factory
        {
        public:
            auto connect_transport(
                const json::v1::object& transport_options,
                const rpc::connection_factory_config::connection_settings& settings,
                std::shared_ptr<rpc::service> service) const -> transport_connect_context override
            {
                auto service_settings = service_settings_from_connection(settings);
                if (service_settings.error_code != rpc::error::OK())
                    return {service_settings.error_code, {}, {}, {}};

                auto materialised
                    = materialise_settings<rpc::sgx_coroutine_transport::transport_settings>(transport_options);
                if (materialised.error_code != rpc::error::OK())
                    return {materialised.error_code, {}, {}, {}};
                auto sgx_settings = std::move(materialised.settings);
                if (sgx_settings.enclave_path.empty())
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                rpc::stream_transport::transport_settings service_transport_settings;
                service_transport_settings.encoding = sgx_settings.encoding;
                auto resolved_service = ensure_service(
                    service_settings.settings, service_transport_settings, std::move(service), "sgx_coroutine_rpc_client");
                if (!resolved_service)
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
                    configured_name(sgx_settings.name, "sgx_coroutine_transport"),
                    resolved_service,
                    std::move(sgx_settings.enclave_path));
                if (sgx_settings.enclave)
                {
                    using json::v1::convert::to_json_object;
                    auto runtime_error
                        = transport->set_enclave_runtime_settings(to_json_object(sgx_settings.enclave.value()));
                    if (runtime_error != rpc::error::OK())
                        return {runtime_error, {}, {}, {}};
                    transport->set_enclave_io_uring_options(sgx_settings.enclave.value().io_uring);
                }
                transport->set_enclave_worker_thread_count(sgx_settings.worker_thread_count);
                transport->set_use_sidecar(sgx_settings.use_sidecar);
                auto startup_error
                    = transport->set_enclave_startup_applications(std::move(sgx_settings.startup_applications));
                if (startup_error != rpc::error::OK())
                    return {startup_error, {}, {}, {}};

                auto proxy_name = configured_name(
                    sgx_settings.service_proxy_name, configured_name(sgx_settings.name, "sgx_coroutine_child"));

                return {rpc::error::OK(), std::move(resolved_service), std::move(transport), std::move(proxy_name)};
            }
        };
    } // namespace

    void register_sgx_coroutine_transport_components(transport_component_map& components)
    {
        components.emplace("sgx_coroutine", std::make_shared<sgx_coroutine_transport_component_factory>());
    }
} // namespace rpc::connection_factory::detail
