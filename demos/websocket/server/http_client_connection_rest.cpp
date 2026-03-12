// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <fmt/format.h>

namespace websocket_demo
{
    namespace v1
    {
        std::string http_client_connection::create_json_response(
            int status_code, const std::string& status_text, const std::string& json_body)
        {
            std::map<std::string, std::string> headers
                = {{"Content-Type", "application/json"}, {"Connection", keep_alive_ ? "keep-alive" : "close"}};

            return build_http_response(status_code, status_text, headers, json_body);
        }

        std::string http_client_connection::create_error_response(int status_code, const std::string& message)
        {
            std::string json_body = fmt::format(R"({{"error":"{}","status":{}}})", message, status_code);
            std::string status_text;

            switch (status_code)
            {
            case 400:
                status_text = "Bad Request";
                break;
            case 404:
                status_text = "Not Found";
                break;
            case 405:
                status_text = "Method Not Allowed";
                break;
            case 500:
                status_text = "Internal Server Error";
                break;
            default:
                status_text = "Error";
                break;
            }

            return create_json_response(status_code, status_text, json_body);
        }

        std::string http_client_connection::create_success_response(const std::string& data)
        {
            std::string json_body = fmt::format(R"({{"success":true,"data":{}}})", data);
            return create_json_response(200, "OK", json_body);
        }

        std::string http_client_connection::handle_rest_request(
            const std::string& method, const std::string& path, const std::string& body)
        {
            if (method == "GET")
            {
                return handle_get(path);
            }
            if (method == "POST")
            {
                return handle_post(path, body);
            }
            if (method == "PUT")
            {
                return handle_put(path, body);
            }
            if (method == "DELETE")
            {
                return handle_delete(path);
            }

            return create_error_response(405, "Method not allowed");
        }

        std::string http_client_connection::handle_get(const std::string& path)
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

        std::string http_client_connection::handle_post(const std::string& path, const std::string& body)
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

        std::string http_client_connection::handle_put(const std::string& path, const std::string& body)
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

        std::string http_client_connection::handle_delete(const std::string& path)
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
