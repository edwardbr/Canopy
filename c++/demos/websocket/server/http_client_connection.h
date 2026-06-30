// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// http_client_connection.h
#pragma once

#include <functional>
#include <optional>
#include <string>

#include <canopy/http_server/http_client_connection.h>
#include <file_system/file_system.h>
#include <memory>

#include <rpc/rpc.h>
#include <streaming/stream.h>

#include <canopy/rest/server.h>

// Forward declarations
namespace websocket_demo
{
    namespace v1
    {
        class http_client_connection
        {
        public:
            using file_system_manager = rpc::shared_ptr<rpc::file_system::i_manager>;
            using websocket_upgrade_handler = canopy::http_server::websocket_handler;

            http_client_connection(
                std::shared_ptr<streaming::stream> stream,
                websocket_upgrade_handler websocket_handler,
                file_system_manager file_system_manager,
                std::string static_root_path,
                canopy::rest::endpoint_registry rest_handlers = {});

            CORO_TASK(std::shared_ptr<rpc::transport>) handle();

        private:
            std::shared_ptr<streaming::stream> stream_;
            websocket_upgrade_handler websocket_handler_;
            file_system_manager file_system_manager_;
            std::string static_root_path_;
            canopy::rest::endpoint_registry rest_handlers_;
        };
    }
}
