/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "transport.h"
#include "address_translator.h"

namespace websocket_demo
{
    namespace v1
    {

        transport::transport(const std::shared_ptr<rpc::service>& service,
            rpc::zone adjacent_zone_id,
            std::shared_ptr<streaming::stream> stream,
            connection_handler&& handler)
            : rpc::transport("websocket", service)
            , stream_(std::move(stream))
            , handler_(std::move(handler))
        {
            set_adjacent_zone_id(adjacent_zone_id);
            // WebSocket transports are immediately available once connection is established
            set_status(rpc::transport_status::CONNECTED);
        }

        CORO_TASK(std::shared_ptr<transport>)
        transport::make_server(const std::shared_ptr<rpc::service>& service,
            const std::shared_ptr<streaming::stream>& stream,
            connection_handler&& handler)
        {
            // generate a zone id for the client
            rpc::get_new_zone_id_params params;
            params.protocol_version = rpc::get_version();
            auto result = co_await service->get_new_zone_id(std::move(params));
            if (result.error_code != rpc::error::OK())
            {
                co_return nullptr;
            }
            rpc::zone client_zone_id = result.zone_id;

            auto transpt = std::shared_ptr<transport>(new transport(service, client_zone_id, stream, std::move(handler)));

            transpt->keep_alive_ = transpt;
            transpt->set_status(rpc::transport_status::CONNECTED);

            service->spawn(transpt->receive_consumer_loop());

            co_return transpt;
        }

        CORO_TASK(void)
        transport::receive_consumer_loop()
        {
            auto self = shared_from_this();
            auto svc = get_service();
            RPC_ASSERT(svc);

            // receive from client — wait for connect_request handshake
            std::array<char, 4096> buf;
            rpc::mutable_byte_span received_span;
            while (get_status() < rpc::transport_status::DISCONNECTED)
            {
                auto [status, span] = co_await stream_->receive(buf, std::chrono::milliseconds{5});
                if (!span.empty())
                {
                    received_span = span;
                    break;
                }
                else if (!status.is_timeout())
                {
                    stream_->set_closed();
                    co_return;
                }
            }

            if (get_status() >= rpc::transport_status::DISCONNECTED)
            {
                co_return;
            }

            // convert buffer to connect_request
            websocket_demo::v1::connect_request req;
            auto parse_err = rpc::from_protobuf<websocket_demo::v1::connect_request>(received_span, req);
            if (!parse_err.empty())
            {
                RPC_ERROR("[WS] invalid connect_request: {}", parse_err);
                stream_->set_closed();
                co_return;
            }

            // make a client object id from the client-supplied object id and the server-assigned zone id
            auto client_object = get_adjacent_zone_id().with_object(req.client_object.object_id);

            rpc::connection_settings cs;
            cs.inbound_interface_id = websocket_demo::v1::i_context_event::get_id(rpc::get_version());
            cs.outbound_interface_id = websocket_demo::v1::i_calculator::get_id(rpc::get_version());
            cs.remote_object_id = client_object;

            RPC_INFO("[WS] Calling handler");

            auto handler_ret = CO_AWAIT handler_(cs, svc, std::static_pointer_cast<transport>(self));
            if (handler_ret.error_code != rpc::error::OK())
            {
                RPC_ERROR("[WS] handler failed: {}", rpc::error::to_string(handler_ret.error_code));
                stream_->set_closed();
                co_return;
            }
            auto output_descr = std::move(handler_ret.output_descriptor);

            // Send connect_response so the client knows the zone/object IDs.
            websocket_demo::v1::connect_response connect_resp;
            connect_resp.client_object = to_object_address(client_object.get_address());
            connect_resp.outbound_remote_object = to_object_address(output_descr.get_address());

            auto resp_payload = rpc::to_protobuf<std::vector<uint8_t>>(connect_resp);
            auto send_status = CO_AWAIT stream_->send(rpc::byte_span{resp_payload});
            if (!send_status.is_ok())
            {
                stream_->set_closed();
                co_return;
            }

            RPC_INFO("[WS] connect_response sent, entering dispatch loop");

            // Main envelope dispatch loop
            while (get_status() < rpc::transport_status::DISCONNECTED)
            {
                auto [recv_status, recv_span] = CO_AWAIT stream_->receive(buf, std::chrono::milliseconds{5});
                if (!recv_span.empty())
                {
                    websocket_demo::v1::envelope env;
                    auto env_err = rpc::from_protobuf<websocket_demo::v1::envelope>(recv_span, env);
                    if (!env_err.empty())
                    {
                        RPC_ERROR("[WS] envelope parse error: {}", env_err);
                        stream_->set_closed();
                        co_return;
                    }
                    CO_AWAIT stub_handle_send(std::move(env));
                }
                else if (!recv_status.is_timeout())
                {
                    break;
                }
            }

            stream_->set_closed();
            RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_subnet());
            CO_RETURN;
        }

        // Outbound i_marshaller interface - sends from child to parent
        CORO_TASK(rpc::send_result)
        transport::outbound_send(rpc::send_params params)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_outbound_send(get_zone_id(),
                    get_adjacent_zone_id(),
                    params.remote_object_id,
                    params.caller_zone_id,
                    params.interface_id,
                    params.method_id);
            }
#endif
            websocket_demo::v1::request request;
            request.encoding = params.encoding_type;
            request.tag = params.tag;
            request.caller_zone_id = params.caller_zone_id;
            request.destination_zone_id = to_object_address(params.remote_object_id.get_address());
            request.interface_id = params.interface_id;
            request.method_id = params.method_id;
            request.data = std::move(params.in_data);
            request.back_channel = std::move(params.in_back_channel);

            auto payload = rpc::to_protobuf<std::vector<char>>(request);
            websocket_demo::v1::envelope envelope;
            envelope.message_type = rpc::id<websocket_demo::v1::request>::get(rpc::get_version());
            envelope.data = std::move(payload);
            auto complete_payload = rpc::to_protobuf(envelope);
            // send to parent

            CO_AWAIT stream_->send(
                rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
            CO_RETURN rpc::send_result{rpc::error::OK(), {}, {}};
        }

        CORO_TASK(void)
        transport::outbound_post(rpc::post_params params)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_outbound_post(get_zone_id(),
                    get_adjacent_zone_id(),
                    params.remote_object_id,
                    params.caller_zone_id,
                    params.interface_id,
                    params.method_id);
            }
#endif
            websocket_demo::v1::request request;
            request.encoding = params.encoding_type;
            request.tag = params.tag;
            request.caller_zone_id = params.caller_zone_id;
            request.destination_zone_id = to_object_address(params.remote_object_id.get_address());
            request.interface_id = params.interface_id;
            request.method_id = params.method_id;
            request.data = std::move(params.in_data);
            request.back_channel = {};

            auto payload = rpc::to_protobuf<std::vector<char>>(request);
            websocket_demo::v1::envelope envelope;
            envelope.message_type = rpc::id<websocket_demo::v1::request>::get(rpc::get_version());
            envelope.data = std::move(payload);
            auto complete_payload = rpc::to_protobuf(envelope);
            // send to parent

            CO_AWAIT stream_->send(
                rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
            CO_RETURN;
        }

        CORO_TASK(rpc::standard_result)
        transport::outbound_try_cast(rpc::try_cast_params params)
        {
            std::ignore = params;
            CO_RETURN rpc::standard_result{.error_code = rpc::error::INCOMPATIBLE_SERVICE(), .out_back_channel = {}};
        }

        CORO_TASK(rpc::standard_result)
        transport::outbound_add_ref(rpc::add_ref_params params)
        {
            std::ignore = params;
            // WebSocket clients do not participate in RPC reference counting lifecycle.
            // Return OK to allow stub registration to succeed.
            CO_RETURN rpc::standard_result{.error_code = rpc::error::OK(), .out_back_channel = {}};
        }

        CORO_TASK(rpc::standard_result)
        transport::outbound_release(rpc::release_params params)
        {
            std::ignore = params;
            CO_RETURN rpc::standard_result{.error_code = rpc::error::OK(), .out_back_channel = {}};
        }

        CORO_TASK(void)
        transport::outbound_object_released(rpc::object_released_params params)
        {
            std::ignore = params;
            CO_RETURN;
        }

        CORO_TASK(void)
        transport::outbound_transport_down(rpc::transport_down_params params)
        {
            std::ignore = params;
            CO_RETURN;
        }

        // Stub handlers (server-side message processing)
        CORO_TASK(void) transport::stub_handle_send(websocket_demo::v1::envelope envelope)
        {
            RPC_DEBUG("stub_handle_send");

            websocket_demo::v1::request request;
            auto error = rpc::from_protobuf<websocket_demo::v1::request>(envelope.data, request);
            if (error.length())
            {
                RPC_DEBUG("Received message ({} bytes) parsing error: {}", envelope.data.size(), error);
                stream_->set_closed();
                CO_RETURN; // no reply.
            }

            auto send_result = CO_AWAIT inbound_send(rpc::send_params{
                .protocol_version = rpc::get_version(),
                .encoding_type = rpc::encoding::protocol_buffers,
                .tag = request.tag,
                .caller_zone_id = get_adjacent_zone_id(),
                .remote_object_id = rpc::remote_object(to_zone_address(request.destination_zone_id)),
                .interface_id = request.interface_id,
                .method_id = request.method_id,
                .in_data = request.data,
                .in_back_channel = {},
            });

            if (send_result.error_code != rpc::error::OK())
            {
                RPC_ERROR("failed send {}", rpc::error::to_string(send_result.error_code));
            }

            // create a response
            websocket_demo::v1::response response;
            response.error = send_result.error_code;
            response.data = std::move(send_result.out_buf);
            response.back_channel = {};

            // convert to protobuf
            auto payload = rpc::to_protobuf<std::vector<char>>(response);

            // make a type determinate envelope
            websocket_demo::v1::envelope response_envelope;
            response_envelope.message_id = envelope.message_id;
            response_envelope.message_type = rpc::id<websocket_demo::v1::response>::get(rpc::get_version());
            response_envelope.data = std::move(payload);

            // convert to protobuf
            auto complete_payload = rpc::to_protobuf(response_envelope);

            CO_AWAIT stream_->send(
                rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));

            RPC_DEBUG("send request complete");
            CO_RETURN;
        }
    }
}
