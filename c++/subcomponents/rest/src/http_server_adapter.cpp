/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <canopy/rest/http_server_adapter.h>

#include <utility>

namespace canopy::rest
{
    server_request to_server_request(const http_server::request& request)
    {
        server_request result;
        result.method = request.method;
        result.target = request.url;
        result.body = request.body;
        result.headers.reserve(request.headers.size());
        for (const auto& [name, value] : request.headers)
            result.headers.push_back({name, value});
        return result;
    }

    http_server::response to_http_response(server_response rest_response)
    {
        http_server::response response;
        response.status_code = rest_response.status_code;
        response.status_text = http_server::status_text(rest_response.status_code);
        response.headers.emplace("Content-Type", std::move(rest_response.content_type));
        for (auto& header : rest_response.headers)
            response.headers.emplace(std::move(header.name), std::move(header.value));
        response.body = std::move(rest_response.body);
        return response;
    }

    CORO_TASK(std::optional<http_server::response>)
    handle_http_request(
        http_server::request request,
        const endpoint_registry& registry) // NOLINT(cppcoreguidelines-avoid-reference-coroutine-parameters): request handler owns registry and immediately awaits the task.
    {
        if (!registry.may_handle(request.url))
            CO_RETURN std::nullopt;

        auto response = CO_AWAIT registry.handle(to_server_request(request));
        if (!response)
            response = error_response(404, "REST endpoint not found");
        CO_RETURN to_http_response(std::move(*response));
    }

} // namespace canopy::rest
