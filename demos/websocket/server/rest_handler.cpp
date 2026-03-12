// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <fmt/format.h>

namespace websocket_demo
{
    namespace v1
    {
        auto http_client_connection::create_error_response(int status_code, const std::string& message)
            -> canopy::http_server::response
        {
            std::string json_body = fmt::format(R"({{"error":"{}","status":{}}})", message, status_code);
            return canopy::http_server::make_json_response(status_code, json_body);
        }

        auto http_client_connection::create_success_response(const std::string& data) -> canopy::http_server::response
        {
            std::string json_body = fmt::format(R"({{"success":true,"data":{}}})", data);
            return canopy::http_server::make_json_response(200, json_body);
        }

        auto http_client_connection::handle_rest_request(const canopy::http_server::request& request)
            -> std::optional<canopy::http_server::response>
        {
            const auto path = canopy::http_server::request_path(request.url);

            if (request.method == "GET")
            {
                return handle_get(path);
            }
            if (request.method == "POST")
            {
                return handle_post(path, request.body);
            }
            if (request.method == "PUT")
            {
                return handle_put(path, request.body);
            }
            if (request.method == "DELETE")
            {
                return handle_delete(path);
            }

            return create_error_response(405, "Method not allowed");
        }

        auto http_client_connection::handle_get(const std::string& path) -> canopy::http_server::response
        {
            if (path == "/api/status")
            {
                return create_success_response("{\"status\":\"running\",\"version\":\"1.0\"}");
            }
            if (path.starts_with("/api/resource/"))
            {
                std::string resource_data = "{\"id\":123,\"name\":\"example\",\"value\":\"data\"}";
                return create_success_response(resource_data);
            }

            return create_error_response(404, "API endpoint not found");
        }

        auto http_client_connection::handle_post(const std::string& path, const std::string& body)
            -> canopy::http_server::response
        {
            if (path == "/api/resource")
            {
                std::string response_data = "{\"id\":456,\"created\":true,\"message\":\"Resource created\"}";
                return create_success_response(response_data);
            }
            if (path.starts_with("/api/"))
            {
                std::string response_data
                    = "{\"message\":\"POST request received\",\"body_length\":" + std::to_string(body.length()) + "}";
                return create_success_response(response_data);
            }

            return create_error_response(404, "API endpoint not found");
        }

        auto http_client_connection::handle_put(const std::string& path, const std::string& body)
            -> canopy::http_server::response
        {
            if (path.starts_with("/api/resource/"))
            {
                std::string response_data = "{\"updated\":true,\"message\":\"Resource updated\"}";
                return create_success_response(response_data);
            }
            if (path.starts_with("/api/"))
            {
                std::string response_data
                    = "{\"message\":\"PUT request received\",\"body_length\":" + std::to_string(body.length()) + "}";
                return create_success_response(response_data);
            }

            return create_error_response(404, "API endpoint not found");
        }

        auto http_client_connection::handle_delete(const std::string& path) -> canopy::http_server::response
        {
            if (path.starts_with("/api/resource/"))
            {
                std::string response_data = "{\"deleted\":true,\"message\":\"Resource deleted\"}";
                return create_success_response(response_data);
            }
            if (path.starts_with("/api/"))
            {
                std::string response_data = "{\"message\":\"DELETE request received\"}";
                return create_success_response(response_data);
            }

            return create_error_response(404, "API endpoint not found");
        }
    }
}
