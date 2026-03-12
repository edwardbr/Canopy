// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "http_client_connection.h"

#include <transports/streaming/transport.h>
#include <websocket_demo/websocket_demo.h>

namespace websocket_demo
{
    namespace v1
    {
        auto http_client_connection::handle_websocket_upgrade(
            const canopy::http_server::request&, std::shared_ptr<streaming::stream> websocket_stream)
            -> coro::task<std::shared_ptr<rpc::stream_transport::transport>>
        {
            auto wsrvc = std::static_pointer_cast<websocket_service>(service_);

            auto transport = rpc::stream_transport::transport::create("websocket",
                service_,
                websocket_stream,
                [wsrvc](const rpc::connection_settings& input_descr,
                    rpc::interface_descriptor& output_descr,
                    std::shared_ptr<rpc::service> svc,
                    std::shared_ptr<rpc::stream_transport::transport> self_transport) -> coro::task<int>
                {
                    RPC_INFO("[WS] Client connecting, zone={}", input_descr.input_zone_id.get_subnet());
                    co_return CO_AWAIT svc
                        ->attach_remote_zone<websocket_demo::v1::i_context_event, websocket_demo::v1::i_calculator>(
                            "websocket",
                            self_transport,
                            input_descr,
                            output_descr,
                            [wsrvc](const rpc::shared_ptr<websocket_demo::v1::i_context_event>& sink,
                                rpc::shared_ptr<websocket_demo::v1::i_calculator>& local,
                                const std::shared_ptr<rpc::service>&) -> coro::task<int>
                            {
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
                                co_return rpc::error::OK();
                            });
                });

            transport->reject_message_type<rpc::stream_transport::init_client_channel_send>();

            transport->add_typed_message_handler<websocket_demo::v1::connect_request>(
                [transport](auto,
                    rpc::stream_transport::envelope_prefix& prefix,
                    rpc::stream_transport::envelope_payload&,
                    websocket_demo::v1::connect_request& request) -> CORO_TASK(rpc::stream_transport::transport::message_hook_result)
                {
                    rpc::zone client_zone_id;
                    std::vector<rpc::back_channel_entry> out_back_channel;
                    auto zone_ret = CO_AWAIT transport->get_service()->get_new_zone_id(
                        prefix.version, client_zone_id, {}, out_back_channel);
                    if (zone_ret != rpc::error::OK())
                    {
                        RPC_ERROR("[WS] Failed to allocate client zone: {}", zone_ret);
                        CO_RETURN rpc::stream_transport::transport::message_hook_result::rejected;
                    }

                    auto callback_object_id = request.inbound_callback_object_id;
                    if (!callback_object_id.is_set())
                    {
                        callback_object_id = request.inbound_remote_object.get_object();
                    }
                    if (!callback_object_id.is_set())
                    {
                        RPC_ERROR("[WS] connect_request missing callback object id");
                        CO_RETURN rpc::stream_transport::transport::message_hook_result::rejected;
                    }

                    const auto inbound_remote_object = client_zone_id.with_object(callback_object_id);
                    rpc::interface_descriptor output_descr;
                    auto ret = CO_AWAIT transport->run_custom_connect(inbound_remote_object,
                        websocket_demo::v1::i_context_event::get_id(prefix.version),
                        websocket_demo::v1::i_calculator::get_id(prefix.version),
                        output_descr);
                    if (ret != rpc::error::OK())
                    {
                        CO_RETURN rpc::stream_transport::transport::message_hook_result::rejected;
                    }

                    transport->send_custom_connect_response<websocket_demo::v1::connect_response>(
                        prefix.version, prefix.sequence_number, client_zone_id, output_descr.destination_zone_id);

                    CO_RETURN rpc::stream_transport::transport::message_hook_result::handled;
                });
            co_return transport;
        }
    }
}
