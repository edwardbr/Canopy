/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <ipc_spsc/config.h>
#include <ipc_spsc/config_schema.h>
#include <transports/ipc_spsc/factory.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class ipc_spsc_component_factory final : public transport_component_factory
        {
        public:
            auto connect_transport(
                const json::v1::object& transport_options,
                const rpc::connection_factory::connection_settings& settings,
                std::shared_ptr<rpc::service> service) const -> transport_connect_context override
            {
                // The IPC SPSC transport owns its base queue stream internally.
                // Applying configured stream layers over that internal stream
                // needs coordinated parent/child startup and is deliberately
                // not enabled until both sides can materialise the same layer stack.
                if (!settings.stream_layers.empty())
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto service_settings = materialise_service_settings(settings);
                if (service_settings.error_code != rpc::error::OK())
                    return {service_settings.error_code, {}, {}, {}};

                auto materialised = materialise_settings<rpc::ipc_spsc::transport_settings>(transport_options);
                if (materialised.error_code != rpc::error::OK())
                    return {materialised.error_code, {}, {}, {}};
                auto ipc_settings = std::move(materialised.settings);

                rpc::stream_transport::transport_settings service_transport_settings;
                service_transport_settings.encoding = ipc_settings.encoding;
                auto resolved_service = ensure_service(
                    service_settings.settings, service_transport_settings, std::move(service), "ipc_spsc_rpc_client");
                if (!resolved_service)
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto result = rpc::ipc_spsc::connect_transport(ipc_settings, std::move(resolved_service));
                return {result.error_code,
                    std::move(result.service),
                    std::move(result.transport),
                    std::move(result.service_proxy_name)};
            }
        };
    } // namespace

    void register_ipc_spsc_components(transport_component_map& components)
    {
        auto factory = std::make_shared<ipc_spsc_component_factory>();
        component_descriptor descriptor{"ipc_spsc",
            component_role::transport,
            component_status::available,
            schema_id("ipc_spsc/config.json"),
            "#/definitions/rpc_ipc_spsc_transport_settings"};
        auto type = descriptor.type;
        components.emplace(std::move(type), transport_component_entry{std::move(descriptor), std::move(factory)});
    }
} // namespace rpc::connection_factory::detail
