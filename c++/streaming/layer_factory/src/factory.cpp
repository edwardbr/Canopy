/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/layer_factory/factory.h>

#include <chrono>
#include <exception>
#include <optional>
#include <utility>

#include <json/convert.h>
#include <json/schema_validator.h>

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET
#  include <streaming/websocket/stream.h>
#  include <websocket_stream/websocket_stream_config.h>
#  include <websocket_stream/websocket_stream_config_schema.h>
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_COMPRESSION
#  include <compression_stream/compression_stream_config.h>
#  include <compression_stream/compression_stream_config_schema.h>
#  include <streaming/compression/stream.h>
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
#  include <streaming/secure_stream.h>
#  ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_OPENSSL_TLS_CONFIG
#    include <openssl_tls_stream/openssl_tls_stream_config.h>
#    include <openssl_tls_stream/openssl_tls_stream_config_schema.h>
#  elif defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_MBEDTLS_CONFIG)
#    include <mbedtls_stream/mbedtls_stream_config.h>
#    include <mbedtls_stream/mbedtls_stream_config_schema.h>
#  endif
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_SPSC_WRAPPING
#  include <spsc_wrapping_stream/spsc_wrapping_stream_config.h>
#  include <spsc_wrapping_stream/spsc_wrapping_stream_config_schema.h>
#  include <streaming/spsc_wrapping/stream.h>
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION
#  include <attestation_stream/attestation_stream_config.h>
#  include <attestation_stream/attestation_stream_config_schema.h>
#  include <streaming/attestation/stream.h>
#endif

namespace streaming::layer_factory
{
    namespace
    {
#if defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS) && defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_OPENSSL_TLS_CONFIG)
        namespace tls_config = ::rpc::openssl_tls_stream;
#elif defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS) && defined(CANOPY_STREAMING_LAYER_FACTORY_HAS_MBEDTLS_CONFIG)
        namespace tls_config = ::rpc::mbedtls_stream;
#endif

        const json::v1::object& empty_layer_settings()
        {
            static const json::v1::object settings(json::v1::map{});
            return settings;
        }

        const json::v1::object& layer_settings_object(const rpc::stream_layers::stream_layer_settings& layer)
        {
            if (!layer.settings)
                return empty_layer_settings();
            return layer.settings.value();
        }

        json::v1::object merge_layer_settings(
            const json::v1::object& base_values,
            const json::v1::object& override_values)
        {
            if (override_values.get_type() == json::v1::object::type::null_type)
                return base_values;

            if (base_values.get_type() != json::v1::object::type::map_type
                || override_values.get_type() != json::v1::object::type::map_type)
            {
                return override_values;
            }

            auto merged = base_values.as_map();
            for (const auto& [key, override_value] : override_values.as_map())
            {
                if (override_value.get_type() == json::v1::object::type::null_type)
                    continue;

                auto existing = merged.find(key);
                if (existing != merged.end())
                {
                    existing->second = merge_layer_settings(existing->second, override_value);
                    continue;
                }

                if (override_value.get_type() == json::v1::object::type::map_type)
                    merged.emplace(key, merge_layer_settings(json::v1::object(json::v1::map{}), override_value));
                else
                    merged.emplace(key, override_value);
            }
            return json::v1::object(std::move(merged));
        }

        template<class Settings> std::optional<Settings> materialise_layer_settings(const json::v1::object& settings)
        {
            try
            {
                static const json::v1::object schema = json::v1::parse(Settings::get_schema(rpc::encoding::yas_json));
                static const json::v1::object defaults = []
                {
                    using json::v1::convert::to_json_object;
                    return to_json_object(Settings{});
                }();
                auto effective_settings = merge_layer_settings(defaults, settings);
                json::v1::schema::schema_validator(schema).validate_or_throw(effective_settings);
                return json::v1::convert::from_json_object<Settings>(effective_settings);
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET
        auto apply_websocket_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer,
            layer_direction direction) -> stream_layer_result
        {
            auto settings
                = materialise_layer_settings<::rpc::websocket_stream::stream_settings>(layer_settings_object(layer));
            if (!settings)
                return {rpc::error::INVALID_DATA(), {}};

            return {rpc::error::OK(),
                std::make_shared<::streaming::websocket::stream>(
                    std::move(stream),
                    std::move(*settings),
                    direction == layer_direction::accept ? ::rpc::websocket_stream::endpoint_role::server
                                                         : ::rpc::websocket_stream::endpoint_role::client)};
        }
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_COMPRESSION
        auto apply_compression_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer) -> stream_layer_result
        {
            auto settings
                = materialise_layer_settings<::rpc::compression_stream::stream_settings>(layer_settings_object(layer));
            if (!settings)
                return {rpc::error::INVALID_DATA(), {}};

            return {rpc::error::OK(),
                std::make_shared<::streaming::compression::stream>(std::move(stream), std::move(*settings))};
        }
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
        using tls_stream_settings = tls_config::stream_settings;
        using tls_config_peer_verification = tls_config::peer_verification;
        using tls_config_pem_credentials = tls_config::pem_credentials;

        auto secure_peer_verification(tls_config_peer_verification value) -> ::streaming::secure::peer_verification
        {
            switch (value)
            {
            case tls_config_peer_verification::optional:
                return ::streaming::secure::peer_verification::optional;
            case tls_config_peer_verification::required:
                return ::streaming::secure::peer_verification::required;
            case tls_config_peer_verification::none:
            default:
                return ::streaming::secure::peer_verification::none;
            }
        }

        auto secure_pem_credentials(const tls_config_pem_credentials& value) -> ::streaming::secure::pem_credentials
        {
            ::streaming::secure::pem_credentials result;
            result.certificate = value.certificate;
            result.private_key = value.private_key;
            result.trust_anchor = value.trust_anchor;
            return result;
        }

        auto make_tls_client_context(const tls_stream_settings& settings)
            -> std::shared_ptr<::streaming::secure::client_context>
        {
            ::streaming::secure::client_context_options options;
            options.verify_peer = settings.client.verify_peer;
            if (!settings.client.trust_anchor.empty())
                return std::make_shared<::streaming::secure::client_context>(settings.client.trust_anchor, options);
            return std::make_shared<::streaming::secure::client_context>(options);
        }

        auto make_tls_server_context(const tls_stream_settings& settings) -> std::shared_ptr<::streaming::secure::context>
        {
            if (!settings.server.credentials)
                return {};

            ::streaming::secure::server_context_options options;
            options.verify_peer = secure_peer_verification(settings.server.verify_peer);
            return std::make_shared<::streaming::secure::context>(
                secure_pem_credentials(settings.server.credentials.value()), options);
        }

        auto apply_tls_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer,
            layer_direction direction,
            const layer_context& context) -> CORO_TASK(stream_layer_result)
        {
            auto settings = materialise_layer_settings<tls_stream_settings>(layer_settings_object(layer));
            if (!settings)
                CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};

            std::shared_ptr<::streaming::secure::stream> tls_stream;
            if (direction == layer_direction::accept)
            {
                auto server_context = context.tls_server_context;
                if (!server_context)
                    server_context = make_tls_server_context(*settings);
                if (!server_context)
                    CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};
                tls_stream = std::make_shared<::streaming::secure::stream>(std::move(stream), std::move(server_context));
                if (!CO_AWAIT tls_stream->handshake())
                    CO_RETURN stream_layer_result{rpc::error::TRANSPORT_ERROR(), {}};
            }
            else
            {
                auto client_context = context.tls_client_context;
                if (!client_context)
                    client_context = make_tls_client_context(*settings);
                tls_stream = std::make_shared<::streaming::secure::stream>(std::move(stream), std::move(client_context));
                if (!CO_AWAIT tls_stream->client_handshake())
                    CO_RETURN stream_layer_result{rpc::error::TRANSPORT_ERROR(), {}};
            }

            CO_RETURN stream_layer_result{rpc::error::OK(), std::move(tls_stream)};
        }
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_SPSC_WRAPPING
        auto apply_spsc_wrapping_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer,
            const layer_context& context) -> stream_layer_result
        {
            auto settings
                = materialise_layer_settings<::rpc::spsc_wrapping_stream::stream_settings>(layer_settings_object(layer));
            if (!settings || !context.stream_scheduler)
                return {rpc::error::INVALID_DATA(), {}};

            return {rpc::error::OK(),
                ::streaming::spsc_wrapping::stream::create(std::move(stream), context.stream_scheduler)};
        }
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION
        auto attestation_service_for(
            const layer_context& context,
            const ::rpc::attestation_stream::stream_settings& settings)
            -> std::shared_ptr<canopy::security::attestation::attestation_service>
        {
            if (settings.service_name)
            {
                const auto found = context.named_attestation_services.find(settings.service_name.value());
                if (found != context.named_attestation_services.end())
                    return found->second;
            }
            return context.attestation_service;
        }

        auto attestation_role(
            const ::rpc::attestation_stream::stream_settings& settings,
            layer_direction direction) -> ::streaming::attestation::handshake_role
        {
            if (!settings.role)
            {
                return direction == layer_direction::accept ? ::streaming::attestation::handshake_role::server
                                                            : ::streaming::attestation::handshake_role::client;
            }

            switch (settings.role.value())
            {
            case ::rpc::attestation_stream::handshake_role::server:
                return ::streaming::attestation::handshake_role::server;
            case ::rpc::attestation_stream::handshake_role::client:
            default:
                return ::streaming::attestation::handshake_role::client;
            }
        }

        auto apply_attestation_layer(
            std::shared_ptr<::streaming::stream> stream,
            const rpc::stream_layers::stream_layer_settings& layer,
            layer_direction direction,
            const layer_context& context) -> CORO_TASK(stream_layer_result)
        {
            auto settings
                = materialise_layer_settings<::rpc::attestation_stream::stream_settings>(layer_settings_object(layer));
            if (!settings)
                CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};

            auto service = attestation_service_for(context, *settings);
            if (!service)
                CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};

            ::streaming::attestation::stream_options options;
            options.service = std::move(service);
            options.transcript_id = settings->transcript_id;
            if (settings->handshake_timeout_ms != 0)
            {
                options.handshake_timeout = std::chrono::milliseconds{
                    static_cast<std::chrono::milliseconds::rep>(settings->handshake_timeout_ms)};
            }

            auto attested_stream
                = std::make_shared<::streaming::attestation::stream>(std::move(stream), std::move(options));
            if (!CO_AWAIT attested_stream->handshake(attestation_role(*settings, direction)))
                CO_RETURN stream_layer_result{rpc::error::TRANSPORT_ERROR(), {}};

            CO_RETURN stream_layer_result{rpc::error::OK(), std::move(attested_stream)};
        }
#endif
    } // namespace

    auto apply_stream_layer(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction) -> stream_layer_result
    {
        if (!stream || layer.type.empty())
            return {rpc::error::INVALID_DATA(), {}};

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET
        if (layer.type == "websocket")
            return apply_websocket_layer(std::move(stream), layer, direction);
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_COMPRESSION
        if (layer.type == "compression" || layer.type == "zstd")
            return apply_compression_layer(std::move(stream), layer);
#endif

        return {rpc::error::INVALID_DATA(), {}};
    }

    auto apply_stream_layers(
        std::shared_ptr<::streaming::stream> stream,
        const std::vector<rpc::stream_layers::stream_layer_settings>& layers,
        size_t first_layer,
        layer_direction direction) -> stream_layer_result
    {
        if (!stream || first_layer > layers.size())
            return {rpc::error::INVALID_DATA(), {}};

        for (auto layer_index = first_layer; layer_index < layers.size(); ++layer_index)
        {
            auto wrapped = apply_stream_layer(std::move(stream), layers[layer_index], direction);
            if (wrapped.error_code != rpc::error::OK())
                return wrapped;
            stream = std::move(wrapped.stream);
        }

        return {rpc::error::OK(), std::move(stream)};
    }

    auto apply_stream_layer_async(
        std::shared_ptr<::streaming::stream> stream,
        const rpc::stream_layers::stream_layer_settings& layer,
        layer_direction direction,
        const layer_context& context) -> CORO_TASK(stream_layer_result)
    {
        if (!stream || layer.type.empty())
            CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_WEBSOCKET
        if (layer.type == "websocket")
            CO_RETURN apply_websocket_layer(std::move(stream), layer, direction);
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_COMPRESSION
        if (layer.type == "compression" || layer.type == "zstd")
            CO_RETURN apply_compression_layer(std::move(stream), layer);
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_TLS
        if (layer.type == "tls")
            CO_RETURN CO_AWAIT apply_tls_layer(std::move(stream), layer, direction, context);
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_SPSC_WRAPPING
        if (layer.type == "spsc_wrapping" || layer.type == "spsc_wrapper")
            CO_RETURN apply_spsc_wrapping_layer(std::move(stream), layer, context);
#endif

#ifdef CANOPY_STREAMING_LAYER_FACTORY_HAS_ATTESTATION
        if (layer.type == "attestation" || layer.type == "attestation_stream")
            CO_RETURN CO_AWAIT apply_attestation_layer(std::move(stream), layer, direction, context);
#endif

        CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};
    }

    auto apply_stream_layers_async(
        std::shared_ptr<::streaming::stream> stream,
        const std::vector<rpc::stream_layers::stream_layer_settings>& layers,
        size_t first_layer,
        layer_direction direction,
        const layer_context& context) -> CORO_TASK(stream_layer_result)
    {
        if (!stream || first_layer > layers.size())
            CO_RETURN stream_layer_result{rpc::error::INVALID_DATA(), {}};

        for (auto layer_index = first_layer; layer_index < layers.size(); ++layer_index)
        {
            auto wrapped = CO_AWAIT apply_stream_layer_async(std::move(stream), layers[layer_index], direction, context);
            if (wrapped.error_code != rpc::error::OK())
                CO_RETURN wrapped;
            stream = std::move(wrapped.stream);
        }

        CO_RETURN stream_layer_result{rpc::error::OK(), std::move(stream)};
    }
} // namespace streaming::layer_factory
