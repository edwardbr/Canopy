/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/connection_factory.h>

#include <json/convert.h>

#include <utility>

#ifdef CANOPY_REST_HAS_TCP_BLOCKING
#  include <tcp_blocking_stream/tcp_blocking_stream_config_schema.h>
#endif

#ifdef CANOPY_REST_HAS_TCP_COROUTINE
#  include <tcp_coroutine_stream/tcp_coroutine_stream_config_schema.h>
#endif

#ifdef CANOPY_REST_HAS_OPENSSL_TLS_CONFIG
#  include <openssl_tls_stream/openssl_tls_stream_config_schema.h>
#elif defined(CANOPY_REST_HAS_MBEDTLS_CONFIG)
#  include <mbedtls_stream/mbedtls_stream_config_schema.h>
#endif

namespace canopy::rest
{
    namespace
    {
        template<class Settings>
        rpc::stream_layers::stream_layer_settings make_layer(
            std::string type,
            const Settings& settings)
        {
            using json::v1::convert::to_json_object;

            rpc::stream_layers::stream_layer_settings layer;
            layer.type = std::move(type);
            layer.settings = to_json_object(settings);
            return layer;
        }

        std::string default_base_stream_type()
        {
#ifdef CANOPY_REST_HAS_TCP_COROUTINE
            return "tcp_coroutine";
#elif defined(CANOPY_REST_HAS_TCP_BLOCKING)
            return "tcp_blocking";
#else
            return {};
#endif
        }

        void append_tcp_base_layer(
            rpc::connection_factory::connection_settings& result,
            const connection_settings& settings,
            const std::string& base_type)
        {
#ifdef CANOPY_REST_HAS_TCP_COROUTINE
            if (base_type == "tcp_coroutine")
            {
                rpc::tcp_coroutine_stream::endpoint endpoint;
                endpoint.host = settings.endpoint.host;
                endpoint.port = effective_port(settings.endpoint);
                endpoint.ipv6 = settings.endpoint.ipv6;
                endpoint.connect_timeout = static_cast<uint64_t>(settings.connect_timeout.count());
                result.stream_layers.push_back(make_layer(base_type, endpoint));
                return;
            }
#endif

#ifdef CANOPY_REST_HAS_TCP_BLOCKING
            if (base_type == "tcp_blocking")
            {
                rpc::tcp_blocking_stream::endpoint endpoint;
                endpoint.host = settings.endpoint.host;
                endpoint.port = effective_port(settings.endpoint);
                endpoint.ipv6 = settings.endpoint.ipv6;
                endpoint.connect_timeout = static_cast<uint64_t>(settings.connect_timeout.count());
                result.stream_layers.push_back(make_layer(base_type, endpoint));
            }
#else
            (void)result;
            (void)settings;
            (void)base_type;
#endif
        }

        void append_tls_layer(
            rpc::connection_factory::connection_settings& result,
            const connection_settings& settings)
        {
            if (!uses_tls(settings.endpoint))
                return;

#ifdef CANOPY_REST_HAS_OPENSSL_TLS_CONFIG
            rpc::openssl_tls_stream::stream_settings tls;
            tls.client.verify_peer = settings.tls.verify_peer;
            tls.client.server_name = settings.endpoint.host;
            tls.client.trust_anchor = settings.tls.trust_anchor;
            tls.client.trust_anchor_file = settings.tls.trust_anchor_file;
            result.stream_layers.push_back(make_layer("tls", tls));
#elif defined(CANOPY_REST_HAS_MBEDTLS_CONFIG)
            rpc::mbedtls_stream::stream_settings tls;
            tls.client.verify_peer = settings.tls.verify_peer;
            tls.client.server_name = settings.endpoint.host;
            tls.client.trust_anchor = settings.tls.trust_anchor;
            tls.client.trust_anchor_file = settings.tls.trust_anchor_file;
            result.stream_layers.push_back(make_layer("tls", tls));
#else
            (void)result;
            (void)settings;
#endif
        }

        struct connection_factory_connector
        {
            connection_factory_settings factory_settings;

            auto operator()(
                connection_settings settings,
                std::shared_ptr<rpc::service> service) const -> CORO_TASK(stream_result)
            {
                auto stream_settings = make_stream_connection_settings(settings, factory_settings);
                if (stream_settings.stream_layers.empty())
                    CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

                auto result = CO_AWAIT rpc::connection_factory::connect_stream(
                    std::move(stream_settings), std::move(service), factory_settings.factory_context);
                CO_RETURN stream_result{result.error_code, std::move(result.stream)};
            }
        };
    } // namespace

    rpc::connection_factory::connection_settings make_stream_connection_settings(
        const connection_settings& settings,
        const connection_factory_settings& factory_settings)
    {
        if (!factory_settings.stream_connection.stream_layers.empty())
            return factory_settings.stream_connection;

        rpc::connection_factory::connection_settings result;
        const auto base_type = factory_settings.base_stream_type.empty() ? default_base_stream_type()
                                                                         : factory_settings.base_stream_type;
        if (base_type.empty())
            return result;

        append_tcp_base_layer(result, settings, base_type);
        append_tls_layer(result, settings);
        return result;
    }

    stream_connector make_connection_factory_connector(connection_factory_settings factory_settings)
    {
        return connection_factory_connector{std::move(factory_settings)};
    }

    void use_connection_factory(
        connection_settings& settings,
        connection_factory_settings factory_settings)
    {
        settings.connector = make_connection_factory_connector(std::move(factory_settings));
    }
} // namespace canopy::rest
