// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <canopy/http_utils/http.h>
#include <canopy/http_server/static_webpage_delivery.h>
#include <canopy/rest/http_server_adapter.h>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            auto has_suffix(
                std::string_view value,
                std::string_view suffix) -> bool
            {
                return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
            }

            auto should_disable_static_cache(const canopy::http_server::request& request) -> bool
            {
                const auto path = canopy::http_utils::request_path(request.url, "");
                if (path.empty() || path == "/" || path == "/index.html" || path == "/client.js")
                {
                    return true;
                }

                return path.rfind("/generated/", 0) == 0 && has_suffix(path, ".js");
            }

            struct rest_request_handler
            {
                canopy::rest::endpoint_registry registry;

                auto operator()(canopy::http_server::request request)
                    -> CORO_TASK(std::optional<canopy::http_server::response>)
                {
                    CO_RETURN CO_AWAIT canopy::rest::handle_http_request(std::move(request), registry);
                }
            };

        }

        http_client_connection::http_client_connection(
            std::shared_ptr<streaming::stream> stream,
            websocket_upgrade_handler websocket_handler,
            file_system_manager file_system_manager,
            std::string static_root_path,
            canopy::rest::endpoint_registry rest_handlers)
            : stream_(std::move(stream))
            , websocket_handler_(std::move(websocket_handler))
            , file_system_manager_(std::move(file_system_manager))
            , static_root_path_(std::move(static_root_path))
            , rest_handlers_(std::move(rest_handlers))
        {
            // rest_handlers_ and websocket_handler_ are both injected at the application boundary. This HTTP session
            // owns protocol wiring, not the concrete RPC object factories behind those handlers.
        }

        CORO_TASK(std::shared_ptr<rpc::transport>)
        http_client_connection::handle()
        {
            canopy::http_server::handler_set handlers;
            canopy::http_server::static_webpage_handler_config webpage_config;
            webpage_config.root_path = static_root_path_;
            webpage_config.file_system = file_system_manager_;
            webpage_config.disable_cache_for_request = should_disable_static_cache;
            handlers.webpage_handler = canopy::http_server::make_static_webpage_handler(std::move(webpage_config));

            // REST dependency injection point. rest_handlers_ is populated by the application from generated
            // Interface::rest_handler_info metadata, then these two callbacks expose that registry to the HTTP server.
            handlers.rest_handler = rest_request_handler{rest_handlers_};
            handlers.is_rest_request = [this](const canopy::http_server::request& request)
            { return rest_handlers_.may_handle(request.url); };
            handlers.websocket_upgrade_handler = websocket_handler_;

            canopy::http_server::client_connection connection(stream_, std::move(handlers));
            CO_RETURN CO_AWAIT connection.handle();
        }
    }
}
