/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <sgx_coroutine_transport/sgx_coroutine_transport_config.h>
#include <sgx_coroutine_transport/sgx_coroutine_transport_config_schema.h>
#include <transports/sgx_coroutine/host/factory.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class sgx_coroutine_transport_component_factory final : public transport_component_factory
        {
        public:
            auto connect_transport(
                const json::v1::object& transport_options,
                const rpc::connection_factory::connection_settings& settings,
                std::shared_ptr<rpc::service> service) const -> transport_connect_context override
            {
                auto service_settings = materialise_service_settings(settings);
                if (service_settings.error_code != rpc::error::OK())
                    return {service_settings.error_code, {}, {}, {}};

                auto materialised
                    = materialise_settings<rpc::sgx_coroutine_transport::transport_settings>(transport_options);
                if (materialised.error_code != rpc::error::OK())
                    return {materialised.error_code, {}, {}, {}};
                auto sgx_settings = std::move(materialised.settings);

                rpc::stream_transport::transport_settings service_transport_settings;
                service_transport_settings.encoding = sgx_settings.encoding;
                auto resolved_service = ensure_service(
                    service_settings.settings, service_transport_settings, std::move(service), "sgx_coroutine_rpc_client");
                if (!resolved_service)
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto result
                    = rpc::sgx_coroutine_transport::host::connect_transport(sgx_settings, std::move(resolved_service));
                return {result.error_code,
                    std::move(result.service),
                    std::move(result.transport),
                    std::move(result.service_proxy_name)};
            }
        };
    } // namespace

    void register_sgx_coroutine_transport_components(transport_component_map& components)
    {
        component_descriptor descriptor{"sgx_coroutine",
            component_role::transport,
            component_status::available,
            schema_id("sgx_coroutine_transport/sgx_coroutine_transport_config.json"),
            "#/definitions/rpc_sgx_coroutine_transport_transport_settings"};
        auto type = descriptor.type;
        components.emplace(
            std::move(type),
            transport_component_entry{std::move(descriptor), std::make_shared<sgx_coroutine_transport_component_factory>()});
    }
} // namespace rpc::connection_factory::detail
