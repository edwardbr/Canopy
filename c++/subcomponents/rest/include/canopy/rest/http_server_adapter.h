/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <optional>

#include <canopy/http_server/http_client_connection.h>
#include <canopy/rest/server.h>

namespace canopy::rest
{
    [[nodiscard]] server_request to_server_request(const http_server::request& request);
    [[nodiscard]] http_server::response to_http_response(server_response response);

    CORO_TASK(std::optional<http_server::response>)
    handle_http_request(
        http_server::request request,
        const endpoint_registry& registry); // NOLINT(cppcoreguidelines-avoid-reference-coroutine-parameters): request handler owns registry and immediately awaits the task.

} // namespace canopy::rest
