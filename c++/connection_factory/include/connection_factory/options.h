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
#include <streaming/stream_layers.h>
#include <stream_transport/stream_transport_config.h>
#include <stream_transport/stream_transport_config_schema.h>
#include <transports/streaming/factory.h>

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

    using stream_rpc_connection_settings = rpc::stream_transport::connection_settings;

    namespace detail
    {
        struct transport_selection_result
        {
            int error_code{rpc::error::OK()};
            std::string type{"stream_rpc"};
            const typed_settings* settings{};
        };

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

        const json::v1::object& settings_object(const typed_settings& typed);
        const json::v1::object& settings_object(const rpc::stream_layers::stream_layer_settings& typed);

        materialise_settings_result<service_settings> service_settings_from_connection(const connection_settings& settings);

        transport_selection_result transport_from_connection(const connection_settings& settings);

        materialise_settings_result<rpc::stream_transport::transport_settings>
        stream_rpc_transport_settings_from_connection(const connection_settings& settings);

        materialise_settings_result<rpc::stream_transport::listener_settings> stream_rpc_listener_settings_from_connection(
            const connection_settings& settings);

        struct resolve_stream_rpc_settings_result
        {
            int error_code{rpc::error::OK()};
            stream_rpc_connection_settings settings;
        };

        resolve_stream_rpc_settings_result resolve_stream_rpc_settings(const connection_settings& settings);

    } // namespace detail

    struct materialise_connection_settings_result
    {
        int error_code{rpc::error::OK()};
        connection_settings settings;
    };

    materialise_connection_settings_result materialise_connection_settings(
        const json::v1::object& client_overrides,
        const json::v1::object& default_values = empty_options());

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
    inline materialise_settings_result<Settings> materialise_settings(const typed_settings& typed)
    {
        return materialise_settings<Settings>(detail::settings_object(typed));
    }

    stream_rpc_connection_settings make_stream_rpc_settings(
        rpc::stream_transport::transport_settings transport = {},
        service_settings service = {},
        rpc::stream_transport::listener_settings listener = {});

    std::optional<rpc::encoding> encoding_option(const rpc::stream_transport::transport_settings& settings);

    std::string configured_name(
        const rpc::optional<std::string>& configured,
        std::string fallback);

    std::string service_name(
        const service_settings& settings,
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

    using rpc::stream_transport::make_default_executor;
} // namespace rpc::connection_factory
