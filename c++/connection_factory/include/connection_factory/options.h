/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <json/config.h>
#include <json/config_loader.h>
#include <connection_factory_config/connection_factory_config.h>
#include <connection_factory_config/connection_factory_config_schema.h>
#include <transports/streaming/transport.h>

namespace rpc::connection_factory
{
    // JSON configuration is materialised once at the API boundary. The factory
    // helpers below consume the generated typed options so schema defaults,
    // application defaults, validation, and conversion are not repeated by
    // every helper in the call chain.

    // Shared empty configuration object used by the public factory defaults.
    // Factories treat this as "only use schema/application defaults".
    inline const json::v1::object& empty_options()
    {
        static const json::v1::object options(json::v1::map{});
        return options;
    }

    namespace detail
    {
        // The connection factory schema is generated from connection_factory_config.idl.
        // It is the contract for accepted option keys; legacy flat aliases are
        // intentionally not accepted here.
        inline const json::v1::object& connection_factory_options_schema()
        {
            static const json::v1::object schema = json::v1::parse(
                rpc::connection_factory_config::stream_factory_options::get_schema(rpc::encoding::yas_json));
            return schema;
        }

        inline const rpc::connection_factory_config::named_options* named_section(
            const rpc::connection_factory_config::stream_factory_options& options,
            std::string_view section)
        {
            if (section == "service" && options.service)
                return &options.service.value();
            if (section == "transport" && options.transport)
                return &options.transport.value();
            if (section == "listener" && options.listener)
                return &options.listener.value();
            if (section == "connection" && options.connection)
                return &options.connection.value();
            return nullptr;
        }
    } // namespace detail

    struct materialise_options_result
    {
        int error_code{rpc::error::OK()};
        rpc::connection_factory_config::stream_factory_options options;
    };

    // Merge order is schema defaults, component defaults, then caller overrides.
    // Validation happens only after the effective object has been assembled.
    inline materialise_options_result materialise_options(
        const json::v1::object& client_overrides,
        const json::v1::object& default_values = empty_options())
    {
        try
        {
            return {rpc::error::OK(),
                json::v1::load_typed_config<rpc::connection_factory_config::stream_factory_options>(
                    detail::connection_factory_options_schema(), default_values, client_overrides)};
        }
        catch (const std::exception&)
        {
            return {rpc::error::INVALID_DATA(), {}};
        }
    }

    inline std::optional<rpc::encoding> encoding_option(
        const rpc::connection_factory_config::stream_factory_options& options);
    inline std::string configured_name(
        const rpc::connection_factory_config::stream_factory_options& options,
        std::string_view section,
        std::string fallback);
    inline rpc::stream_transport::stream_transport_options transport_options(
        const rpc::connection_factory_config::stream_factory_options& options);

    inline std::optional<rpc::encoding> encoding_option(const rpc::connection_factory_config::rpc_options& options)
    {
        if (!options.encoding)
            return std::nullopt;
        if (options.encoding.value() == rpc::encoding::not_set)
            return std::nullopt;
        return options.encoding.value();
    }

    inline std::optional<rpc::encoding> encoding_option(const rpc::connection_factory_config::stream_factory_options& options)
    {
        if (!options.rpc)
            return std::nullopt;
        return encoding_option(options.rpc.value());
    }

    inline std::string configured_name(
        const rpc::connection_factory_config::stream_factory_options& options,
        std::string_view section,
        std::string fallback)
    {
        const auto* named = detail::named_section(options, section);
        if (!named)
            return fallback;
        if (!named->name)
            return fallback;
        return named->name.value();
    }

    inline rpc::stream_transport::stream_transport_options transport_options(
        const rpc::connection_factory_config::stream_factory_options& options)
    {
        rpc::stream_transport::stream_transport_options result;
        if (!options.rpc)
            return result;

        const auto& rpc_options = options.rpc.value();
        if (rpc_options.call_timeout)
            result.call_timeout = std::chrono::milliseconds(rpc_options.call_timeout.value());
        if (rpc_options.call_timeout_sweep)
            result.call_timeout_sweep = std::chrono::milliseconds(rpc_options.call_timeout_sweep.value());
        if (rpc_options.shutdown_timeout)
            result.shutdown_timeout = std::chrono::milliseconds(rpc_options.shutdown_timeout.value());
        return result;
    }
} // namespace rpc::connection_factory
