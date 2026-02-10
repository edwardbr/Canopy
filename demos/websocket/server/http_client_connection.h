// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.h
#pragma once

#include <memory>
#include <string>
#include <map>
#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <llhttp.h>
#include <fmt/format.h>
#include "websocket_service.h"
#include "stream.h"

// Forward declarations
namespace websocket_demo
{
    namespace v1
    {
        class i_calculator;

        class http_client_connection
        {
        public:
            // Constructor takes a stream (can be plain TCP or TLS)
            explicit http_client_connection(std::shared_ptr<stream> stream, std::shared_ptr<websocket_service> service);

            // Main coroutine that handles the connection
            auto handle() -> coro::task<void>;

        private:
            // HTTP parsing context
            struct http_request_context
            {
                std::string method;
                std::string url;
                std::map<std::string, std::string> headers;
                std::string body;
                std::string current_header_field;
                std::string current_header_value;
                bool headers_complete{false};
                bool message_complete{false};
            };

            // llhttp callbacks (static)
            static int on_method(llhttp_t* parser, const char* at, size_t length);
            static int on_url(llhttp_t* parser, const char* at, size_t length);
            static int on_header_field(llhttp_t* parser, const char* at, size_t length);
            static int on_header_value(llhttp_t* parser, const char* at, size_t length);
            static int on_headers_complete(llhttp_t* parser);
            static int on_body(llhttp_t* parser, const char* at, size_t length);
            static int on_message_complete(llhttp_t* parser);

            // HTTP response builders
            std::string build_http_response(int status_code,
                const std::string& status_text,
                const std::map<std::string, std::string>& headers,
                const std::string& body);
            std::string build_websocket_handshake_response(const std::string& accept_key);

            // HTTP helper functions
            std::string get_content_type(const std::string& path);
            std::string serve_file(const std::string& path);

            // REST API handlers
            std::string handle_rest_request(const std::string& method, const std::string& path, const std::string& body);
            std::string handle_get(const std::string& path);
            std::string handle_post(const std::string& path, const std::string& body);
            std::string handle_put(const std::string& path, const std::string& body);
            std::string handle_delete(const std::string& path);

            // JSON response helpers
            std::string create_json_response(int status_code, const std::string& status_text, const std::string& json_body);
            std::string create_error_response(int status_code, const std::string& message);
            std::string create_success_response(const std::string& data);

            // Member data
            std::shared_ptr<stream> stream_;
            std::shared_ptr<websocket_service> service_;
        };
    }
}
