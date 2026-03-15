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
            auto transpt
                = co_await transport::make_server<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                    service_,
                    stream,
                    [](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                        rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                        const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(int)
                    {
                        auto wsrvc = std::static_pointer_cast<websocket_service>(svc);
                        local = wsrvc->get_demo_instance();
                        if (!local)
                            CO_RETURN rpc::error::OBJECT_NOT_FOUND();
                        if (sink)
                        {
                            auto ret = CO_AWAIT local->set_callback(sink);
                            CO_RETURN ret;
                        }
                        CO_RETURN rpc::error::OK();
                    });
            co_return transpt;
        }
    }
}
