// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <websocket_demo/websocket_demo.h>
#include "address_translator.h"
#include "transport.h"

namespace websocket_demo
{
    namespace v1
    {
        auto http_client_connection::handle_websocket_upgrade(const canopy::http_server::request& parsed_request,
            std::shared_ptr<streaming::stream> stream) -> coro::task<std::shared_ptr<rpc::transport>>
        {

            auto handler = [service = service_](const rpc::connection_settings& input_descr,
                               rpc::interface_descriptor& output_interface,
                               std::shared_ptr<rpc::service> child_service_ptr,
                               std::shared_ptr<rpc::transport> transport) -> CORO_TASK(int)
            {
                auto ret
                    = CO_AWAIT service->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                        "websocket",
                        transport,
                        input_descr,
                        output_interface,
                        [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                            rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                            const std::shared_ptr<rpc::service>& svc) -> coro::task<int>
                        {
                            auto wsrvc = std::static_pointer_cast<websocket_service>(svc);
                            local = wsrvc->get_demo_instance();
                            if (!local)
                            {
                                RPC_ERROR("[WS] get_demo_instance returned null");
                                co_return rpc::error::OBJECT_NOT_FOUND();
                            }
                            if (sink)
                            {
                                RPC_INFO("[WS] Calling set_callback");
                                CO_AWAIT local->set_callback(sink);
                                RPC_INFO("[WS] set_callback completed");
                            }
                            co_return 0;
                        });
                CO_RETURN ret;
            };

            // create a websocket transport
            auto transpt = co_await transport::create(service_, stream, std::move(handler));
            co_return transpt;
        }
    }
}
