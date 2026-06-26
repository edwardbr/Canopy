/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/connection.h>

#include <canopy/http_utils/http.h>
#include <canopy/rest/helpers.h>
#include <json/convert.h>

#include <cctype>
#include <string_view>
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
        constexpr auto header_name
            = [](const streaming::http_client::header& header) -> std::string_view { return header.name; };

        bool is_cookie_control(unsigned char ch)
        {
            return ch <= 0x1FU || ch == 0x7FU;
        }

        bool valid_cookie_name(std::string_view name)
        {
            if (name.empty())
                return false;

            for (const auto ch : name)
            {
                const auto value = static_cast<unsigned char>(ch);
                if (is_cookie_control(value) || std::isspace(value) || value == '=' || value == ';')
                    return false;
            }
            return true;
        }

        bool valid_cookie_value(std::string_view value)
        {
            for (const auto ch : value)
            {
                const auto byte = static_cast<unsigned char>(ch);
                if (is_cookie_control(byte) || byte == ';')
                    return false;
            }
            return true;
        }

        int validate_default_cookies(const std::vector<cookie>& cookies)
        {
            for (const auto& item : cookies)
            {
                if (item.name.empty())
                    continue;
                if (!valid_cookie_name(item.name) || !valid_cookie_value(item.value))
                    return rpc::error::INVALID_DATA();
            }
            return rpc::error::OK();
        }

        void append_cookie_header(
            std::vector<streaming::http_client::header>& headers,
            const std::vector<cookie>& cookies)
        {
            if (cookies.empty())
                return;

            std::string value;
            for (const auto& item : cookies)
            {
                if (item.name.empty())
                    continue;
                if (!value.empty())
                    value += "; ";
                value += item.name;
                value.push_back('=');
                value += item.value;
            }

            if (value.empty())
                return;

            auto existing = canopy::http_utils::find_header(headers, "Cookie", header_name);
            if (existing != headers.end())
            {
                if (!existing->value.empty())
                    existing->value += "; ";
                existing->value += value;
                return;
            }

            headers.push_back({"Cookie", std::move(value)});
        }

        void append_default_headers(
            std::vector<streaming::http_client::header>& headers,
            const std::vector<streaming::http_client::header>& defaults)
        {
            for (const auto& header : defaults)
            {
                if (!canopy::http_utils::has_header(headers, header.name, header_name))
                    headers.push_back(header);
            }
        }

        template<class Settings>
        rpc::stream_layers::stream_layer_settings make_layer(
            std::string type,
            const Settings& settings)
        {
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
    } // namespace

    bool uses_tls(const authority& endpoint) noexcept
    {
        return canopy::http_utils::ascii_iequals(endpoint.scheme, "https");
    }

    uint16_t effective_port(const authority& endpoint) noexcept
    {
        if (endpoint.port != 0)
            return endpoint.port;
        return uses_tls(endpoint) ? 443 : 80;
    }

    std::string http_host(const authority& endpoint)
    {
        const auto port = effective_port(endpoint);
        const bool default_port = (uses_tls(endpoint) && port == 443)
                                  || (canopy::http_utils::ascii_iequals(endpoint.scheme, "http") && port == 80);

        std::string host;
        if (endpoint.ipv6)
            host = '[' + endpoint.host + ']';
        else
            host = endpoint.host;

        if (!default_port)
        {
            host.push_back(':');
            host += std::to_string(port);
        }
        return host;
    }

    rpc::connection_factory::connection_settings make_stream_connection_settings(const connection_settings& settings)
    {
        if (!settings.stream_connection.stream_layers.empty())
            return settings.stream_connection;

        rpc::connection_factory::connection_settings result;
        const auto base_type = settings.base_stream_type.empty() ? default_base_stream_type() : settings.base_stream_type;
        if (base_type.empty())
            return result;

        append_tcp_base_layer(result, settings, base_type);
        append_tls_layer(result, settings);
        return result;
    }

    void apply_request_defaults(
        streaming::http_client::request& request,
        const connection_settings& settings)
    {
        if (request.host.empty())
            request.host = http_host(settings.endpoint);

        append_default_headers(request.headers, settings.default_headers);

        append_cookie_header(request.headers, settings.default_cookies);

        for (const auto& parameter : settings.default_query_parameters)
            append_query_parameter(request.target, parameter.name, parameter.value);
    }

    int prepare_request(
        streaming::http_client::request& request,
        const connection_settings& settings)
    {
        const auto cookie_error = validate_default_cookies(settings.default_cookies);
        if (cookie_error != rpc::error::OK())
            return cookie_error;

        apply_request_defaults(request, settings);
        if (settings.before_send)
            return settings.before_send(request);
        return rpc::error::OK();
    }

    CORO_TASK(rpc::connection_factory::stream_result)
    connect_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        rpc::connection_factory::context factory_context)
    {
        if (settings.endpoint.host.empty())
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

        auto stream_settings = make_stream_connection_settings(settings);
        if (stream_settings.stream_layers.empty())
            CO_RETURN rpc::connection_factory::stream_result{rpc::error::INVALID_DATA(), {}};

        CO_RETURN CO_AWAIT rpc::connection_factory::connect_stream(
            stream_settings, std::move(service), std::move(factory_context));
    }
} // namespace canopy::rest
