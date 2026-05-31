/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory_components.h>

#include <memory>
#include <utility>

#include <blocking_dll/config.h>
#include <blocking_dll/config_schema.h>
#include <transports/blocking_dll/factory.h>

namespace rpc::connection_factory::detail
{
    namespace
    {
        class blocking_dll_component_factory final : public transport_component_factory
        {
        public:
            auto connect_transport(
                const json::v1::object& transport_options,
                const rpc::connection_factory::connection_settings& settings,
                std::shared_ptr<rpc::service> service) const -> transport_connect_context override
            {
                if (!settings.stream_layers.empty())
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto service_settings = detail::service_settings_from_connection(settings);
                if (service_settings.error_code != rpc::error::OK())
                    return {service_settings.error_code, {}, {}, {}};

                auto materialised = materialise_settings<rpc::blocking_dll::transport_settings>(transport_options);
                if (materialised.error_code != rpc::error::OK())
                    return {materialised.error_code, {}, {}, {}};
                auto dll_settings = std::move(materialised.settings);

                rpc::stream_transport::transport_settings service_transport_settings;
                service_transport_settings.encoding = dll_settings.encoding;
                auto resolved_service = ensure_service(
                    service_settings.settings, service_transport_settings, std::move(service), "blocking_dll_rpc_client");
                if (!resolved_service)
                    return {rpc::error::INVALID_DATA(), {}, {}, {}};

                auto result = rpc::blocking_dll::connect_transport(dll_settings, std::move(resolved_service));
                return {result.error_code,
                    std::move(result.service),
                    std::move(result.transport),
                    std::move(result.service_proxy_name)};
            }
        };
    } // namespace

    void register_blocking_dll_components(transport_component_map& components)
    {
        components.emplace("blocking_dll", std::make_shared<blocking_dll_component_factory>());
    }
} // namespace rpc::connection_factory::detail
