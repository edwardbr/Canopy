/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <json/config.h>
#include <json/config_loader.h>
#include <json/convert.h>
#include <connection_factory_config/connection_factory_config.h>
#include <connection_factory_config/connection_factory_config_schema.h>
#include <connection_factory_service/connection_factory_service.h>
#include <connection_factory_service/connection_factory_service_schema.h>
#include <streaming/stream_layers.h>
#include <stream_transport/stream_transport_config.h>
#include <stream_transport/stream_transport_config_schema.h>
#include <transports/streaming/transport.h>

namespace rpc::connection_factory
{
    // JSON configuration is materialised once at the API boundary. The
    // connection topology is generic, but implementation settings are generated
    // IDL types owned by the implementation named in typed_settings::type.

    const json::v1::object& empty_options();

    template<class Settings> struct materialise_settings_result
    {
        int error_code{rpc::error::OK()};
        Settings settings;
    };

    struct stream_rpc_connection_settings
    {
        rpc::connection_factory_config::service::settings service;
        rpc::stream_transport::transport_settings transport;
        rpc::stream_transport::listener_settings listener;
    };

    struct transport_selection_result
    {
        int error_code{rpc::error::OK()};
        std::string type{"stream_rpc"};
        const rpc::connection_factory_config::typed_settings* settings{};
    };

    namespace detail
    {
        template<class Settings> auto typed_settings_schema() -> const json::v1::object&
        {
            static const json::v1::object schema = json::v1::parse(Settings::get_schema(rpc::encoding::yas_json));
            return schema;
        }

        template<class Settings> auto typed_settings_defaults() -> const json::v1::object&
        {
            static const json::v1::object defaults = []
            {
                using json::v1::convert::to_json_object;
                return to_json_object(Settings{});
            }();
            return defaults;
        }

    } // namespace detail

    struct materialise_connection_settings_result
    {
        int error_code{rpc::error::OK()};
        rpc::connection_factory_config::connection_settings settings;
    };

    materialise_connection_settings_result materialise_connection_settings(
        const json::v1::object& client_overrides,
        const json::v1::object& default_values = empty_options());

    const json::v1::object& settings_object(const rpc::connection_factory_config::typed_settings& typed);
    const json::v1::object& settings_object(const rpc::stream_layers::stream_layer_settings& typed);

    template<class Settings>
    inline materialise_settings_result<Settings> materialise_settings(const json::v1::object& settings)
    {
        try
        {
            return {rpc::error::OK(),
                json::v1::load_typed_config<Settings>(
                    detail::typed_settings_schema<Settings>(), detail::typed_settings_defaults<Settings>(), settings)};
        }
        catch (const std::exception&)
        {
            return {rpc::error::INVALID_DATA(), {}};
        }
    }

    template<class Settings>
    inline materialise_settings_result<Settings> materialise_settings(
        const rpc::connection_factory_config::typed_settings& typed)
    {
        return materialise_settings<Settings>(settings_object(typed));
    }

    materialise_settings_result<rpc::connection_factory_config::service::settings> service_settings_from_connection(
        const rpc::connection_factory_config::connection_settings& settings);

    transport_selection_result transport_from_connection(
        const rpc::connection_factory_config::connection_settings& settings);

    materialise_settings_result<rpc::stream_transport::transport_settings> stream_rpc_transport_settings_from_connection(
        const rpc::connection_factory_config::connection_settings& settings);

    materialise_settings_result<rpc::stream_transport::listener_settings> stream_rpc_listener_settings_from_connection(
        const rpc::connection_factory_config::connection_settings& settings);

    struct resolve_stream_rpc_settings_result
    {
        int error_code{rpc::error::OK()};
        stream_rpc_connection_settings settings;
    };

    resolve_stream_rpc_settings_result resolve_stream_rpc_settings(
        const rpc::connection_factory_config::connection_settings& settings);

    stream_rpc_connection_settings make_stream_rpc_settings(
        rpc::stream_transport::transport_settings transport = {},
        rpc::connection_factory_config::service::settings service = {},
        rpc::stream_transport::listener_settings listener = {});

    std::optional<rpc::encoding> encoding_option(const rpc::stream_transport::transport_settings& settings);

    std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback);

    std::string service_name(
        const rpc::connection_factory_config::service::settings& settings,
        std::string fallback);

    std::string transport_name(
        const rpc::stream_transport::transport_settings& settings,
        std::string fallback);

    std::string service_proxy_name(
        const rpc::stream_transport::transport_settings& settings,
        std::string fallback);

    std::string listener_name(
        const rpc::stream_transport::listener_settings& settings,
        std::string fallback);

    rpc::stream_transport::stream_transport_options transport_options(
        const rpc::stream_transport::transport_settings& settings);
} // namespace rpc::connection_factory
