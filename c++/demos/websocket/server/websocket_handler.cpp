// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "websocket_handler.h"
#include "demo_zone.h"

#include <utility>

#include <transports/untrusted_web/factory.h>

namespace websocket_demo
{
    namespace v1
    {
        auto make_websocket_upgrade_handler(std::shared_ptr<rpc::service> service) -> canopy::http_server::websocket_handler
        {
            // register the upgrade handler
            return [service = std::move(service)](
                       const canopy::http_server::request& parsed_request,
                       std::shared_ptr<streaming::stream> stream) -> CORO_TASK(std::shared_ptr<rpc::transport>)
            {
                // the incoming http request comes here
                (void)parsed_request;
                rpc::untrusted_web::transport_settings settings;
                settings.inactivity_timeout_ms = 5 * 60 * 1000;

                auto factory = [service = std::move(service)]()
                { return std::static_pointer_cast<websocket_demo::v1::websocket_service>(service)->get_demo_instance(); };

                // bind the upgrade to an RPC handler
                auto accepted
                    = CO_AWAIT rpc::untrusted_web::accept_rpc<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                        stream,
                        [factory = std::move(factory)](
                            const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                            const std::shared_ptr<rpc::service>& svc)
                            -> CORO_TASK(rpc::service_connect_result<websocket_demo::v1::i_calculator>)
                        {
                            // now the websocket connection is established and we have received a call back interface
                            // from the client and we are about to give a local one back to the client
                            (void)svc;
                            auto local = factory();
                            if (!local)
                            {
                                CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                    rpc::error::OBJECT_NOT_FOUND(), {}};
                            }

                            if (sink)
                            {
                                // bind the callback sink interface with this local object
                                auto ret = CO_AWAIT local->set_callback(sink);
                                if (ret != rpc::error::OK())
                                {
                                    RPC_ERROR("set_callback failed with error code {}", websocket_error::to_string(ret));

                                    CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                        rpc::error::INVALID_DATA(), {}};
                                }
                                CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                    rpc::error::OK(), std::move(local)};
                            }
                            CO_RETURN rpc::service_connect_result<websocket_demo::v1::i_calculator>{
                                rpc::error::OK(), std::move(local)};
                        },
                        std::move(settings),
                        service);
                if (accepted.error_code != rpc::error::OK())
                    CO_RETURN nullptr;
                CO_RETURN accepted.transport;
            };
        }
    }
}
