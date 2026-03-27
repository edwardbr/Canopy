// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <websocket_demo/websocket_demo.h>
#include <transports/websocket/transport.h>

namespace websocket_demo
{
    namespace v1
    {
        CORO_TASK(std::shared_ptr<rpc::transport>)
        http_client_connection::handle_websocket_upgrade(
            const canopy::http_server::request& parsed_request,
            std::shared_ptr<streaming::stream> stream)
        {
            auto transpt = CO_AWAIT
                websocket_protocol::transport::make_server<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                    service_,
                    stream,
                    [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                        const std::shared_ptr<rpc::service>& svc)
                        -> CORO_TASK(rpc::service_connect_result<websocket_demo::v1::i_calculator>)
                    {
                        auto wsrvc = std::static_pointer_cast<websocket_service>(svc);
                        auto local = wsrvc->get_demo_instance();
                        if (!local)
                            CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                rpc::error::OBJECT_NOT_FOUND(), {}};
                        if (sink)
                        {
                            auto ret = CO_AWAIT local->set_callback(sink);
                            CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{ret, std::move(local)};
                        }
                        CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                            rpc::error::OK(), std::move(local)};
                    });
            CO_RETURN transpt;
        }
    }
}
