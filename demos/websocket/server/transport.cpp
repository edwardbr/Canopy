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
            rpc::zone client_zone_id;
            std::vector<rpc::back_channel_entry> out_back_channel;
            auto error = co_await service->get_new_zone_id(
                rpc::get_version(), client_zone_id, std::vector<rpc::back_channel_entry>{}, out_back_channel);

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

            rpc::interface_descriptor output_descr;
            auto handler_ret = CO_AWAIT handler_(cs, output_descr, svc, std::static_pointer_cast<transport>(self));
            if (handler_ret != rpc::error::OK())
            {
                RPC_ERROR("[WS] handler failed: {}", rpc::error::to_string(handler_ret));
                stream_->set_closed();
                co_return;
            }

            // Send connect_response so the client knows the zone/object IDs.
            websocket_demo::v1::connect_response connect_resp;
            connect_resp.client_object = to_object_address(client_object.get_address());
            connect_resp.outbound_remote_object = to_object_address(output_descr.destination_zone_id.get_address());

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
        CORO_TASK(int)
        transport::outbound_send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::byte_span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_outbound_send(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
            }
#endif
            websocket_demo::v1::request request;
            request.encoding = encoding;
            request.tag = tag;
            request.caller_zone_id = caller_zone_id;
            request.destination_zone_id = to_object_address(remote_object_id.get_address());
            request.interface_id = interface_id;
            request.method_id = method_id;
            request.data = std::vector<char>{(const char*)in_data.begin(), (const char*)in_data.end()};
            request.back_channel = in_back_channel;

            auto payload = rpc::to_protobuf<std::vector<char>>(request);
            websocket_demo::v1::envelope envelope;
            envelope.message_type = rpc::id<websocket_demo::v1::request>::get(rpc::get_version());
            envelope.data = std::move(payload);
            auto complete_payload = rpc::to_protobuf(envelope);
            // send to parent

            CO_AWAIT stream_->send(
                rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(void)
        transport::outbound_post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::byte_span& in_data,
            const std::vector<rpc::back_channel_entry>& back_channel)
        {

#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_transport_outbound_post(
                    get_zone_id(), get_adjacent_zone_id(), remote_object_id, caller_zone_id, interface_id, method_id);
            }
#endif
            websocket_demo::v1::request request;
            request.encoding = encoding;
            request.tag = tag;
            request.caller_zone_id = caller_zone_id;
            request.destination_zone_id = to_object_address(remote_object_id.get_address());
            request.interface_id = interface_id;
            request.method_id = method_id;
            request.data = std::vector<char>{(const char*)in_data.begin(), (const char*)in_data.end()};
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

        CORO_TASK(int)
        transport::outbound_try_cast(uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
        {

            CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
        }

        CORO_TASK(int)
        transport::outbound_add_ref(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            rpc::requesting_zone requesting_zone_id,
            rpc::add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
        {
            // WebSocket clients do not participate in RPC reference counting lifecycle.
            // Return OK to allow stub registration to succeed.
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(int)
        transport::outbound_release(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel)
        {
            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(void)
        transport::outbound_object_released(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& back_channel)
        {

            CO_RETURN;
        }

        CORO_TASK(void)
        transport::outbound_transport_down(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& back_channel)
        {
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

            std::vector<char> out_buf;
            std::vector<rpc::back_channel_entry> out_back_channel;
            auto ret = CO_AWAIT inbound_send(rpc::get_version(),
                rpc::encoding::protocol_buffers,
                request.tag,
                get_adjacent_zone_id(),
                {to_zone_address(request.destination_zone_id)},
                request.interface_id,
                request.method_id,
                request.data,
                out_buf,
                {},
                out_back_channel);

            if (ret != rpc::error::OK())
            {
                RPC_ERROR("failed send {}", rpc::error::to_string(ret));
            }

            // create a response
            websocket_demo::v1::response response;
            response.error = ret;
            response.data.swap(out_buf);
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
