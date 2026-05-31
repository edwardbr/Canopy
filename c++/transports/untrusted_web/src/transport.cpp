/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/untrusted_web/transport.h>
#include <rpc/internal/address_utils.h>
#include <rpc/internal/serialiser.h>

#include <algorithm>
#include <limits>

#ifndef CANOPY_WEBSOCKET_ENCODING
#  define CANOPY_WEBSOCKET_ENCODING CANOPY_DEFAULT_ENCODING
#endif

namespace rpc::untrusted_web
{
    namespace
    {
        constexpr auto idle_retry_delay = std::chrono::milliseconds{25};

        constexpr rpc::encoding websocket_encoding()
        {
            return CANOPY_WEBSOCKET_ENCODING;
        }

        template<
            class OutputBlob = std::vector<std::uint8_t>,
            typename T>
        OutputBlob encode_transport_message(const T& obj)
        {
            return rpc::serialise<OutputBlob>(obj, websocket_encoding());
        }

        template<typename T>
        std::string decode_transport_message(
            const rpc::byte_span& data,
            T& obj)
        {
            return rpc::deserialise(websocket_encoding(), data, obj);
        }

        auto timeout_from_ms(uint64_t value) -> std::chrono::milliseconds
        {
            const auto max_value = static_cast<uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max());
            return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(std::min(value, max_value))};
        }

        auto receive_buffer_size(const rpc::untrusted_web::transport_settings& settings) -> size_t
        {
            const auto requested = std::max(settings.max_envelope_bytes, settings.max_handshake_bytes);
            if (requested == 0)
                return 1;
            return static_cast<size_t>(
                std::min<uint64_t>(requested, static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        }

        auto timeout_expired(
            std::chrono::steady_clock::time_point started,
            uint64_t timeout_ms) -> bool
        {
            if (timeout_ms == 0)
                return false;
            return std::chrono::steady_clock::now() - started >= timeout_from_ms(timeout_ms);
        }
    }

    transport::transport(
        const std::shared_ptr<rpc::service>& service,
        rpc::zone adjacent_zone_id,
        std::shared_ptr<streaming::stream> stream,
        connection_handler&& handler,
        rpc::untrusted_web::transport_settings settings)
        : rpc::transport(
              "untrusted_web",
              service)
        , stream_(std::move(stream))
        , handler_(std::move(handler))
        , settings_(std::move(settings))
    {
        set_adjacent_zone_id(adjacent_zone_id);
        RPC_DEBUG("New transport connection {}", rpc::to_string(adjacent_zone_id));
        // untrusted_web transports are immediately available once connection is established
        set_status(rpc::transport_status::CONNECTED);
    }

    CORO_TASK(std::shared_ptr<transport>)
    transport::create(
        const std::shared_ptr<rpc::service>& service,
        const std::shared_ptr<streaming::stream>& stream,
        connection_handler&& handler,
        rpc::untrusted_web::transport_settings settings)
    {
        // generate a zone id for the client
        rpc::get_new_zone_id_params params;
        params.protocol_version = rpc::get_version();
        auto result = CO_AWAIT service->get_new_zone_id(std::move(params));
        if (result.error_code != rpc::error::OK())
        {
            CO_RETURN nullptr;
        }
        rpc::zone client_zone_id = result.zone_id;

        auto transpt = std::shared_ptr<transport>(
            new transport(service, client_zone_id, stream, std::move(handler), std::move(settings)));

        transpt->set_status(rpc::transport_status::CONNECTED);

        if (!service->SPAWN(transpt->receive_consumer_loop(std::make_unique<activity_tracker>(transpt, service))))
        {
            RPC_ERROR("[untrusted_web] failed to spawn receive loop");
            transpt->set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN nullptr;
        }

        CO_RETURN transpt;
    }

    bool transport::is_valid(rpc::io_status status)
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
        // Sized for video frames (~30–60 KB encoded VP8) plus headroom; bumped
        // from 4 KB which truncated anything beyond a chat-token payload.
        std::vector<char> buf(receive_buffer_size(settings_));
        const auto receive_timeout = timeout_from_ms(settings_.receive_poll_timeout_ms);
        auto handshake_started = std::chrono::steady_clock::now();
        rpc::mutable_byte_span received_span;
        while (get_status() < rpc::transport_status::DISCONNECTED)
        {
            auto [status, span]
                = CO_AWAIT stream_->receive(rpc::mutable_byte_span{buf.data(), buf.size()}, receive_timeout);

            if (!is_valid(status))
            {
                if (status.is_timeout())
                {
                    if (timeout_expired(handshake_started, settings_.handshake_timeout_ms))
                    {
                        RPC_WARNING("[untrusted_web] closing connection after handshake timeout");
                        CO_RETURN;
                    }
                    if (auto scheduler = svc->get_executor())
                        CO_AWAIT scheduler->schedule_after(idle_retry_delay);
                    continue;
                }
                CO_RETURN;
            }

            if (!span.empty())
            {
                if (span.size() > settings_.max_handshake_bytes)
                {
                    RPC_WARNING("[untrusted_web] closing connection after oversized handshake envelope");
                    CO_RETURN;
                }
                received_span = span;
                break;
            }
        }

        if (get_status() >= rpc::transport_status::DISCONNECTED)
        {
            CO_RETURN;
        }

        // decode the outer envelope
        websocket_protocol::v1::envelope handshake_env;
        auto env_parse_err = decode_transport_message(received_span, handshake_env);
        if (!env_parse_err.empty())
        {
            RPC_ERROR("[untrusted_web] invalid handshake envelope: {}", env_parse_err);
            CO_RETURN;
        }
        if (handshake_env.type != websocket_protocol::v1::message_type::handshake)
        {
            RPC_ERROR(
                "[untrusted_web] expected connect_request envelope type, got {}", static_cast<uint8_t>(handshake_env.type));
            CO_RETURN;
        }

        // decode connect_request from envelope data
        websocket_protocol::v1::connect_request req;
        auto parse_err = decode_transport_message(handshake_env.data, req);
        if (!parse_err.empty())
        {
            RPC_ERROR("[untrusted_web] invalid connect_request: {}", parse_err);
            CO_RETURN;
        }

        // make a client object id from the client-supplied object id and the server-assigned zone id
        auto client_object_r = get_adjacent_zone_id().with_object(req.remote_object_id.object_id);
        if (!client_object_r)
        {
            RPC_ERROR("[untrusted_web] with_object failed: {}", client_object_r.error());
            CO_RETURN;
        }
        auto client_object = std::move(*client_object_r);

        rpc::connection_settings cs;
        cs.inbound_interface_id = req.inbound_interface_id;
        cs.outbound_interface_id = req.outbound_interface_id;
        cs.remote_object_id = client_object;
        cs.encoding_type = websocket_encoding();

        // Immediately inform the client of our zone_id and its fully-populated remote_object_id
        // before invoking connection_handler, so any back-channel calls that arrive during
        // handler execution can be routed correctly.
        {
            websocket_protocol::v1::connect_initial_response initial_resp;
            initial_resp.zone_id = get_zone_id();
            initial_resp.remote_object_id = to_zone_address_args(client_object.get_address());
            auto initial_payload = encode_transport_message<std::vector<char>>(initial_resp);
            websocket_protocol::v1::envelope initial_env;
            initial_env.id = 0;
            initial_env.type = websocket_protocol::v1::message_type::handshake_ack;
            initial_env.data = std::move(initial_payload);
            auto initial_complete = encode_transport_message<std::vector<uint8_t>>(initial_env);
            auto initial_send_status = CO_AWAIT stream_->send(rpc::byte_span{initial_complete});

            if (!is_valid(initial_send_status))
            {
                CO_RETURN;
            }
        }

        RPC_INFO("[untrusted_web] Calling handler");

        auto handler_ret = CO_AWAIT handler_(cs, svc, std::static_pointer_cast<transport>(self));
        if (handler_ret.error_code != rpc::error::OK())
        {
            RPC_ERROR("[untrusted_web] handler failed: {}", rpc::error::to_string(handler_ret.error_code));
            CO_RETURN;
        }
        auto output_descr = std::move(handler_ret.output_descriptor);

        // Send connect_response in an envelope with the server-side callable object.
        websocket_protocol::v1::connect_response connect_resp;
        connect_resp.outbound_remote_object = to_zone_address_args(output_descr.get_address());

        auto resp_payload = encode_transport_message<std::vector<char>>(connect_resp);
        websocket_protocol::v1::envelope resp_env;
        resp_env.id = handshake_env.id;
        resp_env.type = websocket_protocol::v1::message_type::handshake_complete;
        resp_env.data = std::move(resp_payload);
        auto resp_complete = encode_transport_message<std::vector<uint8_t>>(resp_env);
        auto send_status = CO_AWAIT stream_->send(rpc::byte_span{resp_complete});
        if (!is_valid(send_status))
        {
            CO_RETURN;
        }

        RPC_INFO("[untrusted_web] connect_response sent, entering dispatch loop");

        // Main envelope dispatch loop
        auto last_activity = std::chrono::steady_clock::now();
        uint32_t decode_failures = 0;
        while (get_status() < rpc::transport_status::DISCONNECTED)
        {
            auto [recv_status, recv_span]
                = CO_AWAIT stream_->receive(rpc::mutable_byte_span{buf.data(), buf.size()}, receive_timeout);
            if (!recv_span.empty())
            {
                last_activity = std::chrono::steady_clock::now();
                if (recv_span.size() > settings_.max_envelope_bytes)
                {
                    RPC_WARNING("[untrusted_web] closing connection after oversized envelope");
                    CO_RETURN;
                }

                websocket_protocol::v1::envelope env;
                auto env_err = decode_transport_message(recv_span, env);
                if (!env_err.empty())
                {
                    RPC_WARNING("[untrusted_web] envelope parse error: {}", env_err);
                    ++decode_failures;
                    if (decode_failures >= settings_.max_decode_failures || settings_.close_on_protocol_error)
                        CO_RETURN;
                    continue;
                }

                if (env.type != websocket_protocol::v1::message_type::send
                    && env.type != websocket_protocol::v1::message_type::post)
                {
                    RPC_WARNING(
                        "[untrusted_web] unexpected envelope type after handshake: {}", static_cast<uint8_t>(env.type));
                    if (settings_.close_on_protocol_error)
                        CO_RETURN;
                    continue;
                }

                if (env.data.size() > settings_.max_request_payload_bytes)
                {
                    RPC_WARNING("[untrusted_web] closing connection after oversized request payload");
                    CO_RETURN;
                }

                CO_AWAIT stub_handle_send(std::move(env));
            }

            if (!is_valid(recv_status))
            {
                if (recv_status.is_timeout())
                {
                    if (timeout_expired(last_activity, settings_.inactivity_timeout_ms))
                    {
                        RPC_WARNING("[untrusted_web] closing inactive connection");
                        break;
                    }
                    if (auto scheduler = svc->get_executor())
                        CO_AWAIT scheduler->schedule_after(idle_retry_delay);
                    continue;
                }
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
        rpc::transport_down_params params{FLD(protocol_version) rpc::get_version(),
            FLD(destination_zone_id) transport->get_zone_id(),
            FLD(caller_zone_id) transport->get_adjacent_zone_id(),
            FLD(in_back_channel){},
            FLD(payload){}};
        CO_AWAIT svc->transport_down(params);
        CO_RETURN;
    }

    // Outbound i_marshaller interface - sends from child to parent
    CORO_TASK(rpc::send_result)
    transport::outbound_send(rpc::send_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_send(
                {get_zone_id(),
                    get_adjacent_zone_id(),
                    params.remote_object_id,
                    params.caller_zone_id,
                    params.interface_id,
                    params.method_id});
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

        auto payload = encode_transport_message<std::vector<char>>(request);
        websocket_protocol::v1::envelope envelope;
        envelope.type = websocket_protocol::v1::message_type::send;
        envelope.data = std::move(payload);
        auto complete_payload = encode_transport_message(envelope);
        // send to parent

        auto status = CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
        if (!status.is_ok())
        {
            RPC_ERROR("Unable to send data to stream");
            CO_AWAIT stream_->set_closed();
            CO_RETURN rpc::send_result{rpc::error::TRANSPORT_ERROR(), {}, {}};
        }
        CO_RETURN rpc::send_result{rpc::error::OK(), {}, {}};
    }

    CORO_TASK(void)
    transport::outbound_post(rpc::post_params params)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_post(
                {get_zone_id(),
                    get_adjacent_zone_id(),
                    params.remote_object_id,
                    params.caller_zone_id,
                    params.interface_id,
                    params.method_id});
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

        auto payload = encode_transport_message<std::vector<char>>(request);
        websocket_protocol::v1::envelope envelope;
        envelope.type = websocket_protocol::v1::message_type::post;
        envelope.data = std::move(payload);
        auto complete_payload = encode_transport_message(envelope);
        // send to parent

        auto status = CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));
        if (!is_valid(status))
        {
            RPC_ERROR("Unable to post data to stream");
            CO_AWAIT stream_->set_closed();
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

        // MSG_POST envelopes are one-way fire-and-forget: the caller does not
        // register a pending response, so emitting one would log a spurious
        // "no pending request" warning on the client.
        const bool is_post = (envelope.type == websocket_protocol::v1::message_type::post);

        websocket_protocol::v1::request request;
        auto error = decode_transport_message(envelope.data, request);
        if (error.length())
        {
            RPC_DEBUG("Received message ({} bytes) parsing error: {}", envelope.data.size(), error);
            CO_AWAIT stream_->set_closed();
            CO_RETURN; // no reply.
        }

        auto send_result = CO_AWAIT inbound_send(
            rpc::send_params{
                FLD(protocol_version) rpc::get_version(),
                FLD(encoding_type) request.encoding,
                FLD(tag) request.tag,
                FLD(caller_zone_id) get_adjacent_zone_id(),
                FLD(remote_object_id) rpc::remote_object(to_zone_address(request.destination_zone_id)),
                FLD(interface_id) request.interface_id,
                FLD(method_id) request.method_id,
                FLD(in_data) request.data,
                FLD(in_back_channel){},
                FLD(request_id) 0,
            });

        if (send_result.error_code != rpc::error::OK())
        {
            RPC_ERROR("failed send {}", rpc::error::to_string(send_result.error_code));
        }

        if (is_post)
        {
            RPC_DEBUG("post request complete (no response)");
            CO_RETURN;
        }

        // create a response
        websocket_protocol::v1::response response;
        response.error = send_result.error_code;
        response.data = std::move(send_result.out_buf);
        response.back_channel = {};

        // convert to protobuf
        auto payload = encode_transport_message<std::vector<char>>(response);

        // make a type determinate envelope
        websocket_protocol::v1::envelope response_envelope;
        response_envelope.id = envelope.id;
        response_envelope.type = websocket_protocol::v1::message_type::response;
        response_envelope.data = std::move(payload);

        // convert to protobuf
        auto complete_payload = encode_transport_message(response_envelope);

        CO_AWAIT stream_->send(
            rpc::byte_span(reinterpret_cast<const char*>(complete_payload.data()), complete_payload.size()));

        RPC_DEBUG("send request complete");
        CO_RETURN;
    }
}
