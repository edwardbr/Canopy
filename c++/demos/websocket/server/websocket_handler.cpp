// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <websocket_demo/websocket_demo.h>
#include <transports/untrusted_web/factory.h>

namespace websocket_demo
{
    namespace v1
    {
        CORO_TASK(std::shared_ptr<rpc::transport>)
        http_client_connection::handle_websocket_upgrade(
            const canopy::http_server::request& parsed_request,
            std::shared_ptr<streaming::stream> stream)
        {
            (void)parsed_request;
            auto calculator_factory = calculator_factory_;
            rpc::untrusted_web::transport_settings settings;
            settings.inactivity_timeout_ms = 5 * 60 * 1000;
            auto accepted
                = CO_AWAIT rpc::untrusted_web::accept_rpc<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                    stream,
                    [calculator_factory](
                        const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                        const std::shared_ptr<rpc::service>& svc)
                        -> CORO_TASK(rpc::service_connect_result<websocket_demo::v1::i_calculator>)
                    {
                        (void)svc;
                        auto local = calculator_factory ? calculator_factory() : nullptr;
                        if (!local)
                        {
                            CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                rpc::error::OBJECT_NOT_FOUND(), {}};
                        }

                        if (sink)
                        {
                            auto ret = CO_AWAIT local->set_callback(sink);
                            if (ret == rpc::error::OK())
                            {
                                CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                    rpc::error::OK(), std::move(local)};
                            }
                            RPC_ERROR("set_callback failed with error code {}", websocket_error::to_string(ret));

                            CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                rpc::error::INVALID_DATA(), {}};
                        }
                        CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                            rpc::error::OK(), std::move(local)};
                    },
                    std::move(settings),
                    service_);
            if (accepted.error_code != rpc::error::OK())
                CO_RETURN nullptr;
            CO_RETURN accepted.transport;
        }
    }
}
