// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.cpp
#include "http_client_connection.h"
#include "websocket_handshake.h"
#include "ws_client_connection.h"
// #include <websocket_demo/websocket_demo.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>

namespace websocket_demo
{
    namespace v1
    {
        http_client_connection::http_client_connection(
            std::shared_ptr<stream> stream, std::shared_ptr<websocket_service> service)
            : stream_(std::move(stream))
            , service_(std::move(service))
        {
        }

        // llhttp callback implementations
        int http_client_connection::on_method(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->method.assign(at, length);
            return 0;
        }

        int http_client_connection::on_url(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->url.assign(at, length);
            return 0;
        }

        int http_client_connection::on_header_field(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);

            // If we have a pending header value, store the previous header
            if (!ctx->current_header_field.empty() && !ctx->current_header_value.empty())
            {
                ctx->headers[ctx->current_header_field] = ctx->current_header_value;
                ctx->current_header_value.clear();
            }

            ctx->current_header_field.assign(at, length);
            return 0;
        }

        int http_client_connection::on_header_value(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->current_header_value.assign(at, length);
            return 0;
        }

        int http_client_connection::on_headers_complete(llhttp_t* parser)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);

            // Store the last header
            if (!ctx->current_header_field.empty() && !ctx->current_header_value.empty())
            {
                ctx->headers[ctx->current_header_field] = ctx->current_header_value;
                ctx->current_header_field.clear();
                ctx->current_header_value.clear();
            }

            ctx->headers_complete = true;
            return 0;
        }

        int http_client_connection::on_body(llhttp_t* parser, const char* at, size_t length)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->body.append(at, length);
            return 0;
        }

        int http_client_connection::on_message_complete(llhttp_t* parser)
        {
            auto* ctx = static_cast<http_request_context*>(parser->data);
            ctx->message_complete = true;
            return 0;
        }

        // Build a properly formatted HTTP response
        std::string http_client_connection::build_http_response(int status_code,
            const std::string& status_text,
            const std::map<std::string, std::string>& headers,
            const std::string& body)
        {
            // Start with status line
            std::string response = fmt::format("HTTP/1.1 {} {}\r\n", status_code, status_text);

            // Add headers
            for (const auto& [key, value] : headers)
            {
                response += fmt::format("{}: {}\r\n", key, value);
            }

            // Add Content-Length if not already present and there's a body
            if (!body.empty() && headers.find("Content-Length") == headers.end())
            {
                response += fmt::format("Content-Length: {}\r\n", body.length());
            }

            // End headers
            response += "\r\n";

            // Add body
            response += body;

            return response;
        }

        // Build WebSocket handshake response
        std::string http_client_connection::build_websocket_handshake_response(const std::string& accept_key)
        {
            std::map<std::string, std::string> headers
                = {{"Upgrade", "websocket"}, {"Connection", "Upgrade"}, {"Sec-WebSocket-Accept", accept_key}};

            return build_http_response(101, "Switching Protocols", headers, "");
        }

        auto http_client_connection::handle() -> coro::task<void>
        {
            std::string buffer(8192, '\0');

            try
            {
                // 1. Initialize llhttp parser
                llhttp_t parser;
                llhttp_settings_t settings;
                http_request_context ctx;

                llhttp_settings_init(&settings);
                settings.on_method = on_method;
                settings.on_url = on_url;
                settings.on_header_field = on_header_field;
                settings.on_header_value = on_header_value;
                settings.on_headers_complete = on_headers_complete;
                settings.on_body = on_body;
                settings.on_message_complete = on_message_complete;

                llhttp_init(&parser, HTTP_REQUEST, &settings);
                parser.data = &ctx;

                bool is_websocket_upgrade = false;
                do
                {
                    // 2. Read the HTTP request
                    co_await stream_->poll(coro::poll_op::read);
                    auto [rstatus, rspan] = stream_->recv(buffer);

                    if (rstatus != coro::net::recv_status::ok || rspan.empty())
                    {
                        std::cerr << "Failed to read HTTP request" << std::endl;
                        co_return;
                    }

                    // 3. Parse the HTTP request
                    // Debug: print first 100 bytes of received data
                    std::cout << "Received " << rspan.size() << " bytes, first bytes: ";
                    for (size_t i = 0; i < std::min(size_t(100), rspan.size()); i++)
                    {
                        unsigned char c = static_cast<unsigned char>(rspan.data()[i]);
                        if (c >= 32 && c < 127)
                        {
                            std::cout << c;
                        }
                        else
                        {
                            std::cout << "\\x" << std::hex << (int)c << std::dec;
                        }
                    }
                    std::cout << std::endl;

                    enum llhttp_errno err = llhttp_execute(&parser, rspan.data(), rspan.size());
                    if (err == HPE_PAUSED_UPGRADE)
                    {
                        is_websocket_upgrade = true;
                        // Resume parser after upgrade
                        llhttp_resume_after_upgrade(&parser);
                        err = HPE_OK; // Treat as successful parse
                    }
                    else if (err != HPE_OK)
                    {
                        std::cerr << "HTTP parse error: " << llhttp_errno_name(err) << std::endl;
                        std::string response = create_error_response(400, "Bad Request");
                        co_await stream_->poll(coro::poll_op::write);
                        stream_->send(std::span<const char>{response});
                        co_return;
                    }
                } while (!ctx.message_complete);

                // 4. Check for WebSocket upgrade
                if (is_websocket_upgrade)
                {
                    // Handle WebSocket upgrade
                    auto key_it = ctx.headers.find("Sec-WebSocket-Key");
                    if (key_it == ctx.headers.end())
                    {
                        std::cerr << "Missing Sec-WebSocket-Key header" << std::endl;
                        co_return;
                    }

                    std::string accept_key = calculate_ws_accept(key_it->second);
                    std::string handshake_response = build_websocket_handshake_response(accept_key);

                    co_await stream_->poll(coro::poll_op::write);
                    auto [send_status, remaining] = stream_->send(std::span<const char>{handshake_response});
                    if (send_status != coro::net::send_status::ok || !remaining.empty())
                    {
                        std::cerr << "Failed to send WebSocket handshake response" << std::endl;
                        co_return;
                    }
                    std::cout << "WebSocket handshake completed" << std::endl;

                    // Create websocket connection and run the message loop
                    ws_client_connection connection(stream_, service_);
                    co_await connection.run();
                    co_return;
                }

                // 5. Handle regular HTTP request
                std::string method = llhttp_method_name(static_cast<llhttp_method_t>(parser.method));
                std::string path = ctx.url;
                std::cout << "HTTP " << method << " request for: " << path << std::endl;

                std::string response;

                // Check if this is a REST API request
                if (path.starts_with("/api/"))
                {
                    // Handle REST API request
                    response = handle_rest_request(method, path, ctx.body);
                    std::cout << "Handled REST API request: " << method << " " << path << std::endl;
                }
                else
                {
                    // Serve static files (only for GET requests)
                    if (method == std::string("GET"))
                    {
                        // Default to index.html if root is requested
                        if (path == "/" || path.empty())
                        {
                            path = "/index.html";
                        }
                        response = serve_file(path);
                        std::cout << "Served file: " << path << std::endl;
                    }
                    else
                    {
                        response = create_error_response(405, "Only GET method allowed for static files");
                    }
                }

                co_await stream_->poll(coro::poll_op::write);
                auto [send_status, remaining] = stream_->send(std::span<const char>{response});
                if (send_status != coro::net::send_status::ok)
                {
                    std::cerr << "Failed to send HTTP response for: " << path << std::endl;
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "Exception in http_client_connection::handle: " << e.what() << std::endl;
            }

            co_return;
        }

        // Get MIME type based on file extension
        std::string http_client_connection::get_content_type(const std::string& path)
        {
            if (path.ends_with(".html"))
                return "text/html";
            if (path.ends_with(".js"))
                return "application/javascript";
            if (path.ends_with(".css"))
                return "text/css";
            if (path.ends_with(".json"))
                return "application/json";
            if (path.ends_with(".png"))
                return "image/png";
            if (path.ends_with(".jpg") || path.ends_with(".jpeg"))
                return "image/jpeg";
            if (path.ends_with(".gif"))
                return "image/gif";
            return "text/plain";
        }

        // Serve a static file
        std::string http_client_connection::serve_file(const std::string& path)
        {
            namespace fs = std::filesystem;

            // Security: prevent directory traversal
            if (path.find("..") != std::string::npos)
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(403, "Forbidden", headers, "Forbidden");
            }

            // Construct file path (www directory relative to executable or absolute path)
            fs::path www_root = fs::path(__FILE__).parent_path() / "www";
            fs::path file_path = www_root / path.substr(1); // Remove leading '/'

            // Check if file exists
            if (!fs::exists(file_path) || !fs::is_regular_file(file_path))
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(404, "Not Found", headers, "Not Found");
            }

            // Read file content
            std::ifstream file(file_path, std::ios::binary);
            if (!file.is_open())
            {
                std::map<std::string, std::string> headers = {{"Content-Type", "text/plain"}, {"Connection", "close"}};
                return build_http_response(500, "Internal Server Error", headers, "Internal Server Error");
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();

            // Build HTTP response
            std::string content_type = get_content_type(path);
            std::map<std::string, std::string> headers = {{"Content-Type", content_type}, {"Connection", "keep-alive"}};

            return build_http_response(200, "OK", headers, content);
        }

        // Create a JSON HTTP response
        std::string http_client_connection::create_json_response(
            int status_code, const std::string& status_text, const std::string& json_body)
        {
            std::map<std::string, std::string> headers = {{"Content-Type", "application/json"}, {"Connection", "close"}};

            return build_http_response(status_code, status_text, headers, json_body);
        }

        // Create an error JSON response
        std::string http_client_connection::create_error_response(int status_code, const std::string& message)
        {
            // Use fmt::format for safe JSON construction (escaping would be needed for production)
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

        // Create a success JSON response
        std::string http_client_connection::create_success_response(const std::string& data)
        {
            std::string json_body = fmt::format(R"({{"success":true,"data":{}}})", data);
            return create_json_response(200, "OK", json_body);
        }

        // Main REST request handler
        std::string http_client_connection::handle_rest_request(
            const std::string& method, const std::string& path, const std::string& body)
        {
            if (method == "GET")
            {
                return handle_get(path);
            }
            else if (method == "POST")
            {
                return handle_post(path, body);
            }
            else if (method == "PUT")
            {
                return handle_put(path, body);
            }
            else if (method == "DELETE")
            {
                return handle_delete(path);
            }
            else
            {
                return create_error_response(405, "Method not allowed");
            }
        }

        // Handle GET requests
        std::string http_client_connection::handle_get(const std::string& path)
        {
            // Stubbed implementation
            if (path == "/api/status")
            {
                return create_success_response("{\"status\":\"running\",\"version\":\"1.0\"}");
            }
            else if (path.starts_with("/api/resource/"))
            {
                // Example: GET /api/resource/123
                std::string resource_data = "{\"id\":123,\"name\":\"example\",\"value\":\"data\"}";
                return create_success_response(resource_data);
            }
            else
            {
                return create_error_response(404, "API endpoint not found");
            }
        }

        // Handle POST requests
        std::string http_client_connection::handle_post(const std::string& path, const std::string& body)
        {
            // Stubbed implementation
            if (path == "/api/resource")
            {
                // Simulate creating a new resource
                std::string response_data = "{\"id\":456,\"created\":true,\"message\":\"Resource created\"}";
                return create_success_response(response_data);
            }
            else if (path.starts_with("/api/"))
            {
                std::string response_data
                    = "{\"message\":\"POST request received\",\"body_length\":" + std::to_string(body.length()) + "}";
                return create_success_response(response_data);
            }
            else
            {
                return create_error_response(404, "API endpoint not found");
            }
        }

        // Handle PUT requests
        std::string http_client_connection::handle_put(const std::string& path, const std::string& body)
        {
            // Stubbed implementation
            if (path.starts_with("/api/resource/"))
            {
                // Simulate updating a resource
                std::string response_data = "{\"updated\":true,\"message\":\"Resource updated\"}";
                return create_success_response(response_data);
            }
            else if (path.starts_with("/api/"))
            {
                std::string response_data
                    = "{\"message\":\"PUT request received\",\"body_length\":" + std::to_string(body.length()) + "}";
                return create_success_response(response_data);
            }
            else
            {
                return create_error_response(404, "API endpoint not found");
            }
        }

        // Handle DELETE requests
        std::string http_client_connection::handle_delete(const std::string& path)
        {
            // Stubbed implementation
            if (path.starts_with("/api/resource/"))
            {
                // Simulate deleting a resource
                std::string response_data = "{\"deleted\":true,\"message\":\"Resource deleted\"}";
                return create_success_response(response_data);
            }
            else if (path.starts_with("/api/"))
            {
                std::string response_data = "{\"message\":\"DELETE request received\"}";
                return create_success_response(response_data);
            }
            else
            {
                return create_error_response(404, "API endpoint not found");
            }
        }
    }
}
