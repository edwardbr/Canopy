/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <shared_scheduler_dll/config.h>
#include <shared_scheduler_dll/config_schema.h>
#include <transports/shared_scheduler_dll/factory.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class shared_scheduler_dll_component_factory final : public transport_component_factory
        {
        public:
            auto connect_transport(
                const json::v1::object& transport_options,
                const rpc::connection_factory::connection_settings& settings,
                std::shared_ptr<rpc::service> service) const -> transport_connect_context override
            {
                if (!settings.stream_layers.empty())
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto service_settings = materialise_service_settings(settings);
                if (service_settings.error_code != rpc::error::OK())
                    return {service_settings.error_code, {}, {}, {}};

                auto materialised = materialise_settings<rpc::shared_scheduler_dll::transport_settings>(transport_options);
                if (materialised.error_code != rpc::error::OK())
                    return {materialised.error_code, {}, {}, {}};
                auto library_settings = std::move(materialised.settings);

                rpc::stream_transport::transport_settings service_transport_settings;
                service_transport_settings.encoding = library_settings.encoding;
                auto resolved_service = ensure_service(
                    service_settings.settings, service_transport_settings, std::move(service), "shared_scheduler_dll_rpc_client");
                if (!resolved_service)
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto result = rpc::shared_scheduler_dll::connect_transport(library_settings, std::move(resolved_service));
                return {result.error_code,
                    std::move(result.service),
                    std::move(result.transport),
                    std::move(result.service_proxy_name)};
            }
        };
    } // namespace

    void register_shared_scheduler_dll_components(transport_component_map& components)
    {
        auto factory = std::make_shared<shared_scheduler_dll_component_factory>();
        component_descriptor descriptor{"shared_scheduler_dll",
            component_role::transport,
            component_status::available,
            schema_id("shared_scheduler_dll/config.json"),
            "#/definitions/rpc_shared_scheduler_dll_transport_settings"};
        auto type = descriptor.type;
        components.emplace(std::move(type), transport_component_entry{std::move(descriptor), std::move(factory)});
    }
} // namespace rpc::connection_factory::detail
