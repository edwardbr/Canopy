// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#include "websocket_compat_stream.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include <rpc/rpc.h>

namespace websocket_demo
{
    namespace v1
    {
        namespace
        {
            auto parse_zone_string(std::string_view text) -> std::optional<rpc::zone>
            {
                if (text.empty())
                {
                    return std::nullopt;
                }

                const auto slash = text.find('/');
                const auto zone_text = text.substr(0, slash);
                const auto colon = zone_text.find(':');

                uint64_t prefix = 0;
                uint64_t subnet = 0;

                try
                {
                    if (colon == std::string_view::npos)
                    {
                        subnet = static_cast<uint64_t>(std::stoull(std::string(zone_text)));
                    }
                    else
                    {
                        prefix = static_cast<uint64_t>(std::stoull(std::string(zone_text.substr(0, colon))));
                        subnet = static_cast<uint64_t>(std::stoull(std::string(zone_text.substr(colon + 1))));
                    }
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }

                return rpc::zone(rpc::zone_address(prefix, static_cast<uint32_t>(subnet), 0));
            }

            template<class T>
            auto make_transport_message(
                uint64_t sequence_number, rpc::stream_transport::message_direction direction, const T& value)
                -> std::vector<char>
            {
                auto payload = rpc::stream_transport::envelope_payload{
                    .payload_fingerprint = rpc::id<T>::get(rpc::get_version()), .payload = rpc::to_yas_binary(value)};

                auto payload_bytes = rpc::to_yas_binary<std::vector<char>>(payload);
                auto prefix = rpc::stream_transport::envelope_prefix{.version = rpc::get_version(),
                    .direction = direction,
                    .sequence_number = sequence_number,
                    .payload_size = payload_bytes.size()};
                auto prefix_bytes = rpc::to_yas_binary<std::vector<char>>(prefix);

                std::vector<char> out;
                out.reserve(prefix_bytes.size() + payload_bytes.size());
                out.insert(out.end(), prefix_bytes.begin(), prefix_bytes.end());
                out.insert(out.end(), payload_bytes.begin(), payload_bytes.end());
                return out;
            }
        }

        websocket_compat_stream::websocket_compat_stream(std::shared_ptr<streaming::stream> underlying)
            : underlying_(std::move(underlying))
            , raw_receive_buffer_(1024 * 1024)
        {
        }

        auto websocket_compat_stream::receive(std::span<char> buffer, std::chrono::milliseconds timeout)
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
        {
            if (!pending_receive_bytes_.empty())
            {
                CO_RETURN CO_AWAIT drain_receive_buffer(buffer);
            }

            auto [status, bytes] = CO_AWAIT underlying_->receive(raw_receive_buffer_, timeout);
            if (!status.is_ok() || bytes.empty())
            {
                CO_RETURN std::pair{status, std::span<char>{}};
            }

            if (!translate_incoming_message(std::span<const char>(bytes.data(), bytes.size())))
            {
                underlying_->set_closed();
                CO_RETURN std::pair{coro::net::io_status{coro::net::io_status::kind::closed}, std::span<char>{}};
            }

            CO_RETURN CO_AWAIT drain_receive_buffer(buffer);
        }

        auto websocket_compat_stream::send(std::span<const char> buffer) -> coro::task<coro::net::io_status>
        {
            if (!pending_send_prefix_)
            {
                rpc::stream_transport::envelope_prefix prefix;
                auto err = rpc::from_yas_binary(
                    rpc::span(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()), prefix);
                if (!err.empty())
                {
                    RPC_ERROR("[WS] Failed to decode transport prefix in compatibility stream: {}", err);
                    co_return coro::net::io_status{coro::net::io_status::kind::closed};
                }

                pending_send_prefix_ = std::move(prefix);
                co_return coro::net::io_status{coro::net::io_status::kind::ok};
            }

            rpc::stream_transport::envelope_payload payload;
            auto err = rpc::from_yas_binary(
                rpc::span(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()), payload);
            if (!err.empty())
            {
                RPC_ERROR("[WS] Failed to decode transport payload in compatibility stream: {}", err);
                pending_send_prefix_.reset();
                co_return coro::net::io_status{coro::net::io_status::kind::closed};
            }

            auto prefix = *pending_send_prefix_;
            pending_send_prefix_.reset();

            auto translated = translate_outgoing_frame(prefix, payload);
            if (translated.empty())
            {
                co_return coro::net::io_status{coro::net::io_status::kind::ok};
            }

            co_return CO_AWAIT underlying_->send(std::span<const char>(translated.data(), translated.size()));
        }

        bool websocket_compat_stream::is_closed() const
        {
            return underlying_->is_closed();
        }

        void websocket_compat_stream::set_closed()
        {
            underlying_->set_closed();
        }

        streaming::peer_info websocket_compat_stream::get_peer_info() const
        {
            return underlying_->get_peer_info();
        }

        void websocket_compat_stream::queue_receive_bytes(std::vector<char> bytes)
        {
            pending_receive_bytes_.insert(pending_receive_bytes_.end(), bytes.begin(), bytes.end());
        }

        auto websocket_compat_stream::drain_receive_buffer(std::span<char> buffer)
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
        {
            auto to_copy = std::min(buffer.size(), pending_receive_bytes_.size());
            for (size_t i = 0; i < to_copy; ++i)
            {
                buffer[i] = pending_receive_bytes_.front();
                pending_receive_bytes_.pop_front();
            }

            co_return std::pair{coro::net::io_status{coro::net::io_status::kind::ok}, buffer.subspan(0, to_copy)};
        }

        auto websocket_compat_stream::translate_incoming_message(std::span<const char> message) -> bool
        {
            if (!handshake_request_seen_)
            {
                return translate_legacy_connect_request(message);
            }

            websocket_demo::v1::envelope envelope;
            auto error = rpc::from_protobuf<websocket_demo::v1::envelope>(
                {reinterpret_cast<const uint8_t*>(message.data()),
                    reinterpret_cast<const uint8_t*>(message.data()) + message.size()},
                envelope);
            if (!error.empty())
            {
                RPC_ERROR("[WS] Invalid websocket protobuf envelope: {}", error);
                return false;
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::request>::get(rpc::get_version()))
            {
                return translate_legacy_request_envelope(message);
            }

            if (envelope.message_type == rpc::id<websocket_demo::v1::response>::get(rpc::get_version()))
            {
                return translate_legacy_response_envelope(message);
            }

            RPC_ERROR("[WS] Unsupported websocket message type {}", envelope.message_type);
            return false;
        }

        auto websocket_compat_stream::translate_legacy_connect_request(std::span<const char> message) -> bool
        {
            websocket_demo::v1::connect_request request;
            auto error = rpc::from_protobuf<websocket_demo::v1::connect_request>(
                {reinterpret_cast<const uint8_t*>(message.data()),
                    reinterpret_cast<const uint8_t*>(message.data()) + message.size()},
                request);
            if (!error.empty())
            {
                RPC_ERROR("[WS] Invalid websocket connect_request: {}", error);
                return false;
            }

            handshake_request_seen_ = true;
            queue_receive_bytes(make_transport_message(0, rpc::stream_transport::message_direction::send, request));
            return true;
        }

        auto websocket_compat_stream::translate_legacy_request_envelope(std::span<const char> message) -> bool
        {
            websocket_demo::v1::envelope envelope;
            auto envelope_error = rpc::from_protobuf<websocket_demo::v1::envelope>(
                {reinterpret_cast<const uint8_t*>(message.data()),
                    reinterpret_cast<const uint8_t*>(message.data()) + message.size()},
                envelope);
            if (!envelope_error.empty())
            {
                RPC_ERROR("[WS] Invalid request envelope: {}", envelope_error);
                return false;
            }

            websocket_demo::v1::request request;
            auto request_error = rpc::from_protobuf<websocket_demo::v1::request>(envelope.data, request);
            if (!request_error.empty())
            {
                RPC_ERROR("[WS] Invalid request payload: {}", request_error);
                return false;
            }

            auto transport_request = rpc::stream_transport::call_send{.encoding = request.encoding,
                .tag = request.tag,
                .caller_zone_id = request.caller_zone_id,
                .destination_zone_id = request.destination_zone_id,
                .interface_id = request.interface_id,
                .method_id = request.method_id,
                .payload = request.data,
                .back_channel = request.back_channel};

            if (!request.caller_zone_id_text.empty())
            {
                if (auto caller_zone = parse_zone_string(request.caller_zone_id_text))
                {
                    transport_request.caller_zone_id = *caller_zone;
                }
            }

            if (!request.destination_zone_id_text.empty())
            {
                if (auto destination_zone = parse_zone_string(request.destination_zone_id_text))
                {
                    transport_request.destination_zone_id
                        = destination_zone->with_object(rpc::object(request.destination_object_id));
                }
            }

            queue_receive_bytes(make_transport_message(
                envelope.message_id, rpc::stream_transport::message_direction::send, transport_request));
            return true;
        }

        auto websocket_compat_stream::translate_legacy_response_envelope(std::span<const char> message) -> bool
        {
            websocket_demo::v1::envelope envelope;
            auto envelope_error = rpc::from_protobuf<websocket_demo::v1::envelope>(
                {reinterpret_cast<const uint8_t*>(message.data()),
                    reinterpret_cast<const uint8_t*>(message.data()) + message.size()},
                envelope);
            if (!envelope_error.empty())
            {
                RPC_ERROR("[WS] Invalid response envelope: {}", envelope_error);
                return false;
            }

            websocket_demo::v1::response response;
            auto response_error = rpc::from_protobuf<websocket_demo::v1::response>(envelope.data, response);
            if (!response_error.empty())
            {
                RPC_ERROR("[WS] Invalid response payload: {}", response_error);
                return false;
            }

            auto transport_response = rpc::stream_transport::call_receive{.payload = response.data,
                .back_channel = response.back_channel,
                .err_code = static_cast<int>(response.error)};

            queue_receive_bytes(make_transport_message(
                envelope.message_id, rpc::stream_transport::message_direction::receive, transport_response));
            return true;
        }

        auto websocket_compat_stream::translate_outgoing_frame(const rpc::stream_transport::envelope_prefix& prefix,
            const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>
        {
            if (payload.payload_fingerprint == rpc::id<websocket_demo::v1::connect_response>::get(prefix.version))
            {
                return translate_connect_response(payload);
            }

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::call_receive>::get(prefix.version))
            {
                return translate_call_receive(prefix, payload);
            }

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::addref_send>::get(prefix.version))
            {
                return synthesize_addref_receive(prefix, payload);
            }

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::call_send>::get(prefix.version))
            {
                return translate_post_send(prefix, payload);
            }

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::post_send>::get(prefix.version))
            {
                return translate_post_send(prefix, payload);
            }

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::release_send>::get(prefix.version)
                || payload.payload_fingerprint == rpc::id<rpc::stream_transport::object_released_send>::get(prefix.version)
                || payload.payload_fingerprint == rpc::id<rpc::stream_transport::transport_down_send>::get(prefix.version))
            {
                return {};
            }

            return {};
        }

        auto websocket_compat_stream::translate_connect_response(const rpc::stream_transport::envelope_payload& payload)
            -> std::vector<char>
        {
            websocket_demo::v1::connect_response response;
            auto error = rpc::from_yas_binary(rpc::span(payload.payload), response);
            if (!error.empty())
            {
                RPC_ERROR("[WS] Failed to decode connect_response payload: {}", error);
                return {};
            }

            auto bytes = rpc::to_protobuf<std::vector<char>>(response);
            return bytes;
        }

        auto websocket_compat_stream::synthesize_addref_receive(const rpc::stream_transport::envelope_prefix& prefix,
            const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>
        {
            rpc::stream_transport::addref_send request;
            auto error = rpc::from_yas_binary(rpc::span(payload.payload), request);
            if (!error.empty())
            {
                RPC_ERROR("[WS] Failed to decode addref_send payload: {}", error);
                return {};
            }

            std::ignore = request;
            queue_receive_bytes(make_transport_message(prefix.sequence_number,
                rpc::stream_transport::message_direction::receive,
                rpc::stream_transport::addref_receive{.back_channel = {}, .err_code = rpc::error::OK()}));
            return {};
        }

        auto websocket_compat_stream::translate_call_receive(const rpc::stream_transport::envelope_prefix& prefix,
            const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>
        {
            rpc::stream_transport::call_receive response;
            auto error = rpc::from_yas_binary(rpc::span(payload.payload), response);
            if (!error.empty())
            {
                RPC_ERROR("[WS] Failed to decode call_receive payload: {}", error);
                return {};
            }

            websocket_demo::v1::response ws_response;
            ws_response.error = response.err_code;
            ws_response.data = std::move(response.payload);
            ws_response.back_channel = std::move(response.back_channel);

            websocket_demo::v1::envelope envelope;
            envelope.message_id = prefix.sequence_number;
            envelope.message_type = rpc::id<websocket_demo::v1::response>::get(prefix.version);
            envelope.data = rpc::to_protobuf<std::vector<char>>(ws_response);
            return rpc::to_protobuf<std::vector<char>>(envelope);
        }

        auto websocket_compat_stream::translate_post_send(const rpc::stream_transport::envelope_prefix& prefix,
            const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>
        {
            websocket_demo::v1::request ws_request;

            if (payload.payload_fingerprint == rpc::id<rpc::stream_transport::call_send>::get(prefix.version))
            {
                rpc::stream_transport::call_send request;
                auto error = rpc::from_yas_binary(rpc::span(payload.payload), request);
                if (!error.empty())
                {
                    RPC_ERROR("[WS] Failed to decode call_send payload: {}", error);
                    return {};
                }

                ws_request.encoding = request.encoding;
                ws_request.tag = request.tag;
                ws_request.caller_zone_id = request.caller_zone_id;
                ws_request.destination_zone_id = request.destination_zone_id;
                ws_request.interface_id = request.interface_id;
                ws_request.method_id = request.method_id;
                ws_request.data = std::move(request.payload);
                ws_request.back_channel = std::move(request.back_channel);
                ws_request.caller_zone_id_text = std::to_string(request.caller_zone_id);
                ws_request.destination_zone_id_text = std::to_string(request.destination_zone_id.as_zone());
                ws_request.destination_object_id = request.destination_zone_id.get_object().get_val();
            }
            else
            {
                rpc::stream_transport::post_send request;
                auto error = rpc::from_yas_binary(rpc::span(payload.payload), request);
                if (!error.empty())
                {
                    RPC_ERROR("[WS] Failed to decode post_send payload: {}", error);
                    return {};
                }

                ws_request.encoding = request.encoding;
                ws_request.tag = request.tag;
                ws_request.caller_zone_id = request.caller_zone_id;
                ws_request.destination_zone_id = request.destination_zone_id;
                ws_request.interface_id = request.interface_id;
                ws_request.method_id = request.method_id;
                ws_request.data = std::move(request.payload);
                ws_request.back_channel = std::move(request.back_channel);
                ws_request.caller_zone_id_text = std::to_string(request.caller_zone_id);
                ws_request.destination_zone_id_text = std::to_string(request.destination_zone_id.as_zone());
                ws_request.destination_object_id = request.destination_zone_id.get_object().get_val();
            }

            websocket_demo::v1::envelope envelope;
            envelope.message_id = prefix.sequence_number;
            envelope.message_type = rpc::id<websocket_demo::v1::request>::get(prefix.version);
            envelope.data = rpc::to_protobuf<std::vector<char>>(ws_request);
            return rpc::to_protobuf<std::vector<char>>(envelope);
        }
    }
}
