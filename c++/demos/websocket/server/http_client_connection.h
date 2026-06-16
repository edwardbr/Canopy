// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.h
#pragma once

#include <optional>
#include <functional>
#include <string>

#include <canopy/http_server/http_client_connection.h>
#include <file_system/file_system.h>
#include <memory>

#include <rpc/rpc.h>
#include <streaming/stream.h>
#include <websocket_demo/websocket_demo.h>

#ifndef CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
#  include "demo_zone.h"
#endif

// Forward declarations
namespace websocket_demo
{
    namespace v1
    {
        class http_client_connection
        {
        public:
            using calculator_factory = std::function<rpc::shared_ptr<i_calculator>()>;
            using file_system_manager = rpc::shared_ptr<rpc::file_system::i_manager>;

#ifndef CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
            explicit http_client_connection(
                std::shared_ptr<streaming::stream> stream,
                std::shared_ptr<websocket_service> service,
                file_system_manager file_system_manager,
                std::string static_root_path);
#endif
            http_client_connection(
                std::shared_ptr<streaming::stream> stream,
                std::shared_ptr<rpc::service> service,
                calculator_factory factory,
                file_system_manager file_system_manager,
                std::string static_root_path);

            CORO_TASK(std::shared_ptr<rpc::transport>) handle();

        private:
            // in rest_handler
            auto handle_rest_request(const canopy::http_server::request& request)
                -> std::optional<canopy::http_server::response>;
            auto handle_get(const std::string& path) -> canopy::http_server::response;
            auto handle_post(
                const std::string& path,
                const std::string& body) -> canopy::http_server::response;
            auto handle_put(
                const std::string& path,
                const std::string& body) -> canopy::http_server::response;
            auto handle_delete(const std::string& path) -> canopy::http_server::response;

            // in websocket_handler
            CORO_TASK(std::shared_ptr<rpc::transport>)
            handle_websocket_upgrade(
                const canopy::http_server::request& request,
                std::shared_ptr<streaming::stream> websocket_stream);

            auto create_error_response(
                int status_code,
                const std::string& message) -> canopy::http_server::response;
            auto create_success_response(const std::string& data) -> canopy::http_server::response;

            std::shared_ptr<streaming::stream> stream_;
            std::shared_ptr<rpc::service> service_;
            calculator_factory calculator_factory_;
            file_system_manager file_system_manager_;
            std::string static_root_path_;
        };
    }
}
