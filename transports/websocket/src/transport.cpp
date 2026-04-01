/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/websocket/transport.h>
#include <rpc/internal/address_utils.h>

namespace websocket_protocol
{

    transport::transport(
        const std::shared_ptr<rpc::service>& service,
        rpc::zone adjacent_zone_id,
        std::shared_ptr<streaming::stream> stream,
        connection_handler&& handler)
        : rpc::transport(
              "websocket",
              service)
        , stream_(std::move(stream))
        , handler_(std::move(handler))
    {
        set_adjacent_zone_id(adjacent_zone_id);
        // WebSocket transports are immediately available once connection is established
        set_status(rpc::transport_status::CONNECTED);
    }

    CORO_TASK(std::shared_ptr<transport>)
    transport::make_server(
        const std::shared_ptr<rpc::service>& service,
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

        transpt->set_status(rpc::transport_status::CONNECTED);

        service->spawn(transpt->receive_consumer_loop(std::make_unique<activity_tracker>(transpt, service)));

        co_return transpt;
    }

    bool transport::is_valid(coro::net::io_status status)
    {
        if (!stream_)
        {
            return false;
        }

        if (status.is_closed())
        {
            return false;
        }

        if (!status.is_ok())
        {
            return false;
        }
        return true;
    }

    CORO_TASK(void)
    transport::receive_consumer_loop(std::unique_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);

        // receive from client — wait for connect_request handshake envelope
        std::array<char, 4096> buf;
        rpc::mutable_byte_span received_span;
        while (get_status() < rpc::transport_status::DISCONNECTED)
        {
            auto [status, span] = co_await stream_->receive(buf, std::chrono::milliseconds{5});

            if (!is_valid(status))
            {
                co_return;
            }

            if (!span.empty())
            {
                received_span = span;
                break;
            }
        }

        if (get_status() >= rpc::transport_status::DISCONNECTED)
        {
            co_return;
        }

        // decode the outer envelope
        websocket_protocol::v1::envelope handshake_env;
        auto env_parse_err = rpc::from_protobuf<websocket_protocol::v1::envelope>(received_span, handshake_env);
        if (!env_parse_err.empty())
        {
            RPC_ERROR("[WS] invalid handshake envelope: {}", env_parse_err);
            co_return;
        }
        if (handshake_env.type != websocket_protocol::v1::message_type::handshake)
        {
            RPC_ERROR("[WS] expected connect_request envelope type, got {}", static_cast<uint8_t>(handshake_env.type));
            co_return;
        }

        // decode connect_request from envelope data
        websocket_protocol::v1::connect_request req;
        auto parse_err = rpc::from_protobuf<websocket_protocol::v1::connect_request>(handshake_env.data, req);
        if (!parse_err.empty())
        {
            RPC_ERROR("[WS] invalid connect_request: {}", parse_err);
            co_return;
        }

        // make a client object id from the client-supplied object id and the server-assigned zone id
        auto client_object_r = get_adjacent_zone_id().with_object(req.remote_object_id.object_id);
        if (!client_object_r)
        {
            RPC_ERROR("[WS] with_object failed: {}", client_object_r.error());
            co_return;
        }
        auto client_object = std::move(*client_object_r);

        rpc::connection_settings cs;
        cs.inbound_interface_id = req.inbound_interface_id;
        cs.outbound_interface_id = req.outbound_interface_id;
        cs.remote_object_id = client_object;

        // Immediately inform the client of our zone_id and its fully-populated remote_object_id
        // before invoking connection_handler, so any back-channel calls that arrive during
        // handler execution can be routed correctly.
        {
            websocket_protocol::v1::connect_initial_response initial_resp;
            initial_resp.zone_id = get_zone_id();
            initial_resp.remote_object_id = to_zone_address_args(client_object.get_address());
            auto initial_payload = rpc::to_protobuf<std::vector<char>>(initial_resp);
            websocket_protocol::v1::envelope initial_env;
            initial_env.id = 0;
            initial_env.type = websocket_protocol::v1::message_type::handshake_ack;
            initial_env.data = std::move(initial_payload);
            auto initial_complete = rpc::to_protobuf<std::vector<uint8_t>>(initial_env);
            auto initial_send_status = CO_AWAIT stream_->send(rpc::byte_span{initial_complete});

            if (!is_valid(initial_send_status))
            {
                co_return;
            }
        }

        RPC_INFO("[WS] Calling handler");

        auto handler_ret = CO_AWAIT handler_(cs, svc, std::static_pointer_cast<transport>(self));
        if (handler_ret.error_code != rpc::error::OK())
        {
            RPC_ERROR("[WS] handler failed: {}", rpc::error::to_string(handler_ret.error_code));
            co_return;
        }
        auto output_descr = std::move(handler_ret.output_descriptor);

        // Send connect_response in an envelope with the server-side callable object.
        websocket_protocol::v1::connect_response connect_resp;
        connect_resp.outbound_remote_object = to_zone_address_args(output_descr.get_address());

        auto resp_payload = rpc::to_protobuf<std::vector<char>>(connect_resp);
        websocket_protocol::v1::envelope resp_env;
        resp_env.id = handshake_env.id;
        resp_env.type = websocket_protocol::v1::message_type::handshake_complete;
        resp_env.data = std::move(resp_payload);
        auto resp_complete = rpc::to_protobuf<std::vector<uint8_t>>(resp_env);
        auto send_status = CO_AWAIT stream_->send(rpc::byte_span{resp_complete});
        if (!is_valid(send_status))
        {
            co_return;
        }

        RPC_INFO("[WS] connect_response sent, entering dispatch loop");

        // Main envelope dispatch loop
        while (get_status() < rpc::transport_status::DISCONNECTED)
        {
            auto [recv_status, recv_span] = CO_AWAIT stream_->receive(buf, std::chrono::milliseconds{5});
            if (!recv_span.empty())
            {
                websocket_protocol::v1::envelope env;
                auto env_err = rpc::from_protobuf<websocket_protocol::v1::envelope>(recv_span, env);
                if (!env_err.empty())
                {
                    RPC_ERROR("[WS] envelope parse error: {}", env_err);
                    co_return;
                }
                CO_AWAIT stub_handle_send(std::move(env));
            }

            if (!is_valid(recv_status))
            {
                break;
            }
        }

        tracker.reset();
        RPC_DEBUG("receive_consumer_loop exiting for zone {}", get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::cleanup(
        std::shared_ptr<transport> transport,
        std::shared_ptr<rpc::service> svc)
    {
        transport->set_status(rpc::transport_status::DISCONNECTED);

        RPC_DEBUG("Both loops completed, finalising transport for zone {}", transport->get_zone_id().get_subnet());
        if (transport->stream_)
        {
            CO_AWAIT transport->stream_->set_closed();
            transport->stream_.reset();
        }
        rpc::transport_down_params params{.protocol_version = rpc::get_version(),
            .destination_zone_id = transport->get_zone_id(),
            .caller_zone_id = transport->get_adjacent_zone_id(),
            .in_back_channel = {}};
        co_await svc->transport_down(params);
        co_return;
    }

    // Outbound i_marshaller interface - sends from child to parent
    CORO_TASK(rpc::send_result)
    transport::outbound_send(rpc::send_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_send(
                get_zone_id(),
                get_adjacent_zone_id(),
                params.remote_object_id,
                params.caller_zone_id,
                params.interface_id,
                params.method_id);
        }
#endif
        if (stream_->is_closed())
        {
            RPC_ERROR("stream already closed");
            CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }

        websocket_protocol::v1::request request;
        request.encoding = params.encoding_type;
        request.tag = params.tag;
        request.caller_zone_id = to_zone_address_args(params.caller_zone_id.get_address());
        request.destination_zone_id = to_zone_address_args(params.remote_object_id.get_address());
        request.interface_id = params.interface_id;
        request.method_id = params.method_id;
        request.data = std::move(params.in_data);
        request.back_channel = std::move(params.in_back_channel);

        auto payload = rpc::to_protobuf<std::vector<char>>(request);
        websocket_protocol::v1::envelope envelope;
        envelope.type = websocket_protocol::v1::message_type::send;
        envelope.data = std::move(payload);
        auto complete_payload = rpc::to_protobuf(envelope);
        // send to parent

        auto status = CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
        if (!status.is_ok())
        {
            RPC_ERROR("Unable to send data to stream");
            co_await stream_->set_closed();
            CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }
        CO_RETURN rpc::send_result{rpc::error::OK(), {}, {}};
    }

    CORO_TASK(void)
    transport::outbound_post(rpc::post_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                get_zone_id(),
                get_adjacent_zone_id(),
                params.remote_object_id,
                params.caller_zone_id,
                params.interface_id,
                params.method_id);
        }
#endif
        if (stream_->is_closed())
        {
            RPC_ERROR("stream already closed");
            CO_RETURN;
        }
        websocket_protocol::v1::request request;
        request.encoding = params.encoding_type;
        request.tag = params.tag;
        request.caller_zone_id = to_zone_address_args(params.caller_zone_id.get_address());
        request.destination_zone_id = to_zone_address_args(params.remote_object_id.get_address());
        request.interface_id = params.interface_id;
        request.method_id = params.method_id;
        request.data = std::move(params.in_data);
        request.back_channel = {};

        auto payload = rpc::to_protobuf<std::vector<char>>(request);
        websocket_protocol::v1::envelope envelope;
        envelope.type = websocket_protocol::v1::message_type::post;
        envelope.data = std::move(payload);
        auto complete_payload = rpc::to_protobuf(envelope);
        // send to parent

        auto status = CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
        if (!is_valid(status))
        {
            RPC_ERROR("Unable to post data to stream");
            co_await stream_->set_closed();
        }
        CO_RETURN;
    }

    CORO_TASK(rpc::standard_result)
    transport::outbound_try_cast(rpc::try_cast_params params)
    {
        std::ignore = params;
        CO_RETURN rpc::standard_result(rpc::error::INCOMPATIBLE_SERVICE(), {});
    }

    CORO_TASK(rpc::standard_result)
    transport::outbound_add_ref(rpc::add_ref_params params)
    {
        std::ignore = params;
        // WebSocket clients do not participate in RPC reference counting lifecycle.
        // Return OK to allow stub registration to succeed.
        CO_RETURN rpc::standard_result(rpc::error::OK(), {});
    }

    CORO_TASK(rpc::standard_result)
    transport::outbound_release(rpc::release_params params)
    {
        std::ignore = params;
        CO_RETURN rpc::standard_result(rpc::error::OK(), {});
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
    CORO_TASK(void) transport::stub_handle_send(websocket_protocol::v1::envelope envelope)
    {
        RPC_DEBUG("stub_handle_send");

        websocket_protocol::v1::request request;
        auto error = rpc::from_protobuf<websocket_protocol::v1::request>(envelope.data, request);
        if (error.length())
        {
            RPC_DEBUG("Received message ({} bytes) parsing error: {}", envelope.data.size(), error);
            co_await stream_->set_closed();
            CO_RETURN; // no reply.
        }

        auto send_result = CO_AWAIT inbound_send(
            rpc::send_params{
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
        websocket_protocol::v1::response response;
        response.error = send_result.error_code;
        response.data = std::move(send_result.out_buf);
        response.back_channel = {};

        // convert to protobuf
        auto payload = rpc::to_protobuf<std::vector<char>>(response);

        // make a type determinate envelope
        websocket_protocol::v1::envelope response_envelope;
        response_envelope.id = envelope.id;
        response_envelope.type = websocket_protocol::v1::message_type::response;
        response_envelope.data = std::move(payload);

        // convert to protobuf
        auto complete_payload = rpc::to_protobuf(response_envelope);

        CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));

        RPC_DEBUG("send request complete");
        CO_RETURN;
    }
}
