// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.h
#pragma once

#include <optional>

#include <canopy/http_server/http_client_connection.h>
#include <memory>

#include <streaming/stream.h>

#include "demo_zone.h"

// Forward declarations
namespace websocket_demo
{
    namespace v1
    {
        class http_client_connection
        {
        public:
            explicit http_client_connection(
                std::shared_ptr<streaming::stream> stream, std::shared_ptr<websocket_service> service);

            CORO_TASK(std::shared_ptr<rpc::transport>) handle();

        private:
            // in rest_handler
            auto handle_rest_request(const canopy::http_server::request& request)
                -> std::optional<canopy::http_server::response>;
            auto handle_get(const std::string& path) -> canopy::http_server::response;
            auto handle_post(const std::string& path, const std::string& body) -> canopy::http_server::response;
            auto handle_put(const std::string& path, const std::string& body) -> canopy::http_server::response;
            auto handle_delete(const std::string& path) -> canopy::http_server::response;

            // in websocket_handler
            CORO_TASK(std::shared_ptr<rpc::transport>) handle_websocket_upgrade(const canopy::http_server::request& request,
                std::shared_ptr<streaming::stream> websocket_stream);

            auto create_error_response(int status_code, const std::string& message) -> canopy::http_server::response;
            auto create_success_response(const std::string& data) -> canopy::http_server::response;

            std::shared_ptr<streaming::stream> stream_;
            std::shared_ptr<websocket_service> service_;
        };
    }
}
