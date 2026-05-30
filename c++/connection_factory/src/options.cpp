/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/options.h>

#include <chrono>
#include <exception>
#include <string_view>

namespace rpc::connection_factory
{
    namespace
    {
        auto connection_settings_schema() -> const json::v1::object&
        {
            static const json::v1::object schema
                = json::v1::parse(connection_settings::get_schema(rpc::encoding::yas_json));
            return schema;
        }

        bool type_is(
            const typed_settings& typed,
            std::string_view expected)
        {
            return typed.type == expected;
        }
    } // namespace

    const json::v1::object& empty_options()
    {
        static const json::v1::object options(json::v1::map{});
        return options;
    }

    materialise_connection_settings_result materialise_connection_settings(
        const json::v1::object& client_overrides,
        const json::v1::object& default_values)
    {
        try
        {
            return {rpc::error::OK(),
                json::v1::load_typed_config<connection_settings>(
                    connection_settings_schema(), default_values, client_overrides)};
        }
        catch (const std::exception&)
        {
            return {rpc::error::INVALID_DATA(), {}};
        }
    }

    const json::v1::object& detail::settings_object(const typed_settings& typed)
    {
        if (!typed.settings)
            return empty_options();
        return typed.settings.value();
    }

    const json::v1::object& detail::settings_object(const rpc::stream_layers::stream_layer_settings& typed)
    {
        if (!typed.settings)
            return empty_options();
        return typed.settings.value();
    }

    materialise_settings_result<service_settings> detail::service_settings_from_connection(
        const connection_settings& settings)
    {
        if (!settings.service)
            return {rpc::error::OK(), {}};
        const auto& typed = settings.service.value();
        if (!type_is(typed, "service") && !type_is(typed, "root_service"))
            return {rpc::error::INVALID_DATA(), {}};
        return materialise_settings<service_settings>(typed);
    }

    detail::transport_selection_result detail::transport_from_connection(const connection_settings& settings)
    {
        if (!settings.transport)
            return {};
        if (settings.transport->type.empty())
            return {rpc::error::INVALID_DATA(), {}, {}};
        return {rpc::error::OK(), settings.transport->type, &settings.transport.value()};
    }

    materialise_settings_result<rpc::stream_transport::transport_settings>
    detail::stream_rpc_transport_settings_from_connection(const connection_settings& settings)
    {
        using transport_settings = rpc::stream_transport::transport_settings;
        auto transport = detail::transport_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            return {transport.error_code, {}};
        if (transport.type == "stream_rpc" && !transport.settings)
            return {rpc::error::OK(), {}};
        if (transport.type != "stream_rpc" || !transport.settings)
            return {rpc::error::INVALID_DATA(), {}};
        return materialise_settings<transport_settings>(*transport.settings);
    }

    materialise_settings_result<rpc::stream_transport::listener_settings>
    detail::stream_rpc_listener_settings_from_connection(const connection_settings& settings)
    {
        using listener_settings = rpc::stream_transport::listener_settings;
        if (!settings.listener)
            return {rpc::error::OK(), {}};
        const auto& typed = settings.listener.value();
        if (!type_is(typed, "stream_rpc"))
            return {rpc::error::INVALID_DATA(), {}};
        return materialise_settings<listener_settings>(typed);
    }

    detail::resolve_stream_rpc_settings_result detail::resolve_stream_rpc_settings(const connection_settings& settings)
    {
        detail::resolve_stream_rpc_settings_result result;
        auto service = detail::service_settings_from_connection(settings);
        if (service.error_code != rpc::error::OK())
            return {service.error_code, {}};
        result.settings.service.name = std::move(service.settings.name);

        auto transport = detail::stream_rpc_transport_settings_from_connection(settings);
        if (transport.error_code != rpc::error::OK())
            return {transport.error_code, {}};
        result.settings.transport = std::move(transport.settings);

        auto listener = detail::stream_rpc_listener_settings_from_connection(settings);
        if (listener.error_code != rpc::error::OK())
            return {listener.error_code, {}};
        result.settings.listener = std::move(listener.settings);
        return result;
    }

    stream_rpc_connection_settings make_stream_rpc_settings(
        rpc::stream_transport::transport_settings transport,
        service_settings service,
        rpc::stream_transport::listener_settings listener)
    {
        rpc::stream_transport::service_settings stream_service;
        stream_service.name = std::move(service.name);
        return rpc::stream_transport::make_connection_settings(
            std::move(transport), std::move(stream_service), std::move(listener));
    }

    std::optional<rpc::encoding> encoding_option(const rpc::stream_transport::transport_settings& settings)
    {
        return rpc::stream_transport::encoding_option(settings);
    }

    std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback)
    {
        return rpc::stream_transport::configured_name(configured, std::move(fallback));
    }

    std::string service_name(
        const service_settings& settings,
        std::string fallback)
    {
        return configured_name(settings.name, std::move(fallback));
    }

    std::string transport_name(
        const rpc::stream_transport::transport_settings& settings,
        std::string fallback)
    {
        return rpc::stream_transport::transport_name(settings, std::move(fallback));
    }

    std::string service_proxy_name(
        const rpc::stream_transport::transport_settings& settings,
        std::string fallback)
    {
        return rpc::stream_transport::service_proxy_name(settings, std::move(fallback));
    }

    std::string listener_name(
        const rpc::stream_transport::listener_settings& settings,
        std::string fallback)
    {
        return rpc::stream_transport::listener_name(settings, std::move(fallback));
    }

    rpc::stream_transport::stream_transport_options transport_options(
        const rpc::stream_transport::transport_settings& settings)
    {
        return rpc::stream_transport::transport_options(settings);
    }
} // namespace rpc::connection_factory
