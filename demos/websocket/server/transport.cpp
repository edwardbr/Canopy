/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <websocket_demo/websocket_demo.h>
#include "transport.h"

#include <wslay/wslay.h>
#include <fmt/format.h>

namespace websocket_demo
{
    transport::transport(std::string name, std::shared_ptr<rpc::service> service, rpc::zone adjacent_zone_id)
        : rpc::transport(name, service, adjacent_zone_id)
    {
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    transport::transport(std::string name, rpc::zone zone_id, rpc::zone adjacent_zone_id)
        : rpc::transport(name, zone_id, adjacent_zone_id)
    {
        // Local transports are always immediately available (in-process)
        set_status(rpc::transport_status::CONNECTED);
    }

    transport::transport(wslay_event_context_ptr wslay_ctx, std::shared_ptr<rpc::service> service, rpc::zone adjacent_zone_id)
        : rpc::transport("websocket", service, adjacent_zone_id)
        , wslay_ctx_(wslay_ctx)
    {
        // WebSocket transports are immediately available once connection is established
        set_status(rpc::transport_status::CONNECTED);
    }

    // Outbound i_marshaller interface - sends from child to parent
    CORO_TASK(int)
    transport::outbound_send(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_transport_outbound_send(
                get_zone_id(), get_adjacent_zone_id(), destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif
        v1::request request;
        request.encoding = encoding;
        request.tag = tag;
        request.caller_zone_id = caller_zone_id.get_val();
        request.destination_zone_id = destination_zone_id.get_val();
        request.object_id = object_id.get_val();
        request.method_id = method_id.get_val();
        request.data = std::vector<char>{(const char*)in_data.begin, (const char*)in_data.end};
        request.back_channel = in_back_channel;

        auto payload = rpc::to_protobuf<std::vector<char>>(request);
        v1::envelope envelope;
        envelope.message_type = rpc::id<v1::request>::get(rpc::get_version());
        envelope.data = std::move(payload);
        auto complete_payload = rpc::to_protobuf(envelope);
        // send to parent

        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(void)
    transport::outbound_post(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::caller_zone caller_zone_id,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& back_channel)
    {
        CO_RETURN;
    }

    CORO_TASK(int)
    transport::outbound_try_cast(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {

        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(int)
    transport::outbound_add_ref(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::known_direction_zone known_direction_zone_id,
        rpc::add_ref_options build_out_param_channel,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(int)
    transport::outbound_release(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
        rpc::caller_zone caller_zone_id,
        rpc::release_options options,
        uint64_t& reference_count,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
    }

    CORO_TASK(void)
    transport::outbound_object_released(uint64_t protocol_version,
        rpc::destination_zone destination_zone_id,
        rpc::object object_id,
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
    CORO_TASK(void) transport::stub_handle_send(v1::envelope envelope)
    {
        RPC_DEBUG("stub_handle_send");

        v1::request request;
        auto error = rpc::from_protobuf<v1::request>(envelope.data, request);
        if (error.length())
        {
            auto reason = fmt::format("invalid message format {}", error);
            RPC_DEBUG("Received message ({} bytes) parsing error: {}", envelope.data.size(), error);

            wslay_event_queue_close(wslay_ctx_,
                WSLAY_CODE_INVALID_FRAME_PAYLOAD_DATA, // 1007
                reinterpret_cast<const uint8_t*>(reason.data()),
                reason.size());
            CO_RETURN; // no reply.
        }

        std::vector<char> out_buf;
        std::vector<rpc::back_channel_entry> out_back_channel;
        // Call inbound_send for routing - transport will route to correct destination
        auto ret = CO_AWAIT inbound_send(rpc::get_version(),
            rpc::encoding::protocol_buffers,
            request.tag,
            get_adjacent_zone_id().as_caller(),
            1,
            1,
            v1::i_calculator::get_id(rpc::get_version()),
            {request.method_id},
            request.data,
            out_buf,
            {},
            out_back_channel);

        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed send");
        }

        // create a response
        v1::response response;
        response.error = ret;
        response.data.swap(out_buf);
        response.back_channel = {};

        // convert to protobuf
        auto payload = rpc::to_protobuf<std::vector<char>>(response);

        // make a type determinate envelope
        v1::envelope response_envelope;
        response_envelope.message_id = envelope.message_id;
        response_envelope.message_type = rpc::id<v1::response>::get(rpc::get_version());
        response_envelope.data = std::move(payload);

        // convert to protobuf
        auto complete_payload = rpc::to_protobuf(response_envelope);

        // Queue the response message for sending back to the client
        if (wslay_ctx_)
        {
            wslay_event_msg msg;
            msg.opcode = WSLAY_BINARY_FRAME;
            msg.msg = complete_payload.data();
            msg.msg_length = complete_payload.size();
            wslay_event_queue_msg(wslay_ctx_, &msg);
            wslay_event_send(wslay_ctx_);
        }

        RPC_DEBUG("send request complete");
        CO_RETURN;
    }
}
