// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <canopy/http_server/static_webpage_delivery.h>

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
                const auto path = canopy::http_server::request_path(request.url);
                if (path.empty() || path == "/" || path == "/index.html" || path == "/client.js")
                {
                    return true;
                }

                return path.rfind("/generated/", 0) == 0 && has_suffix(path, ".js");
            }

            auto disable_static_cache(canopy::http_server::response& response) -> void
            {
                response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0";
                response.headers["Pragma"] = "no-cache";
                response.headers["Expires"] = "0";
            }
        }

#ifndef CANOPY_WEBSOCKET_DEMO_CALCULATOR_ONLY
        http_client_connection::http_client_connection(
            std::shared_ptr<streaming::stream> stream,
            std::shared_ptr<websocket_service> service,
            file_system_manager file_system_manager,
            std::string static_root_path)
            : http_client_connection(
                  std::move(stream),
                  service,
                  [service] { return service ? service->get_demo_instance() : nullptr; },
                  std::move(file_system_manager),
                  std::move(static_root_path))
        {
        }
#endif

        http_client_connection::http_client_connection(
            std::shared_ptr<streaming::stream> stream,
            std::shared_ptr<rpc::service> service,
            calculator_factory factory,
            file_system_manager file_system_manager,
            std::string static_root_path)
            : stream_(std::move(stream))
            , service_(std::move(service))
            , calculator_factory_(std::move(factory))
            , file_system_manager_(std::move(file_system_manager))
            , static_root_path_(std::move(static_root_path))
        {
        }

        CORO_TASK(std::shared_ptr<rpc::transport>)
        http_client_connection::handle()
        {
            auto webpage_delivery = std::make_shared<canopy::http_server::static_webpage_delivery>(
                static_root_path_,
                [file_system_manager = file_system_manager_](
                    std::string file_path, std::vector<uint8_t>& data) -> CORO_TASK(int)
                {
                    if (!file_system_manager)
                    {
                        CO_RETURN rpc::error::INVALID_DATA();
                    }
                    CO_RETURN CO_AWAIT file_system_manager->read_file(std::move(file_path), data);
                });

            canopy::http_server::handler_set handlers;
            handlers.webpage_handler
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                = [webpage_delivery](
                      const canopy::http_server::request& request) -> CORO_TASK(std::optional<canopy::http_server::response>)
            {
                auto response = CO_AWAIT webpage_delivery->handle(request);
                if (response && should_disable_static_cache(request))
                {
                    disable_static_cache(*response);
                }
                CO_RETURN response;
            };
            handlers.rest_handler
                = [this](const canopy::http_server::request& request) { return handle_rest_request(request); };
            handlers.websocket_upgrade_handler
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                = [this](
                      const canopy::http_server::request& request,
                      std::shared_ptr<streaming::stream> websocket_stream) -> CORO_TASK(std::shared_ptr<rpc::transport>)
            { CO_RETURN CO_AWAIT handle_websocket_upgrade(request, websocket_stream); };

            canopy::http_server::client_connection connection(stream_, std::move(handlers));
            CO_RETURN CO_AWAIT connection.handle();
        }
    }
}
