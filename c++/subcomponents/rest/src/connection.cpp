/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/connection.h>

#include <canopy/http_utils/http.h>
#include <canopy/rest/helpers.h>

#include <cctype>
#include <string_view>
#include <utility>

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

    CORO_TASK(stream_result)
    connect_stream(
        const connection_settings& settings,
        std::shared_ptr<rpc::service> service)
    {
        if (settings.endpoint.host.empty())
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

        if (!settings.connector)
            CO_RETURN stream_result{rpc::error::INVALID_DATA(), {}};

        CO_RETURN CO_AWAIT settings.connector(settings, std::move(service));
    }
} // namespace canopy::rest
