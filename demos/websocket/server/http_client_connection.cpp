// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <filesystem>

#include <canopy/http_server/static_webpage_delivery.h>

namespace websocket_demo
{
    namespace v1
    {
        http_client_connection::http_client_connection(
            std::shared_ptr<streaming::stream> stream, std::shared_ptr<websocket_service> service)
            : stream_(std::move(stream))
            , service_(std::move(service))
        {
        }

        auto http_client_connection::handle() -> coro::task<std::shared_ptr<rpc::transport>>
        {
            auto webpage_delivery = std::make_shared<canopy::http_server::static_webpage_delivery>(
                std::filesystem::path(__FILE__).parent_path() / "www");

            canopy::http_server::handler_set handlers;
            handlers.webpage_handler = [webpage_delivery](const canopy::http_server::request& request)
            {
                // handle web page delivery
                return webpage_delivery->handle(request);
            };
            handlers.rest_handler = [this](const canopy::http_server::request& request)
            {
                // handle REST delivery
                return handle_rest_request(request);
            };
            handlers.websocket_upgrade_handler
                = [this](const canopy::http_server::request& request,
                      std::shared_ptr<streaming::stream> websocket_stream) -> coro::task<std::shared_ptr<rpc::transport>>
            {
                // handle web socket delivery
                co_return CO_AWAIT handle_websocket_upgrade(request, websocket_stream);
            };

            canopy::http_server::client_connection connection(stream_, std::move(handlers));
            co_return CO_AWAIT connection.handle();
        }
    }
}
