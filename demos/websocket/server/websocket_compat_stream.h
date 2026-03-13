// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <deque>
#include <optional>
#include <vector>

#include <rpc/rpc.h>
#include <stream_transport/stream_transport.h>
#include <streaming/stream.h>
#include <streaming/ws_stream.h>
#include <websocket_demo/websocket_demo.h>

namespace websocket_demo
{
    namespace v1
    {
        class websocket_compat_stream : public streaming::stream
        {
        public:
            explicit websocket_compat_stream(std::shared_ptr<streaming::stream> underlying);

            auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

            auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;

            bool is_closed() const override;
            void set_closed() override;
            streaming::peer_info get_peer_info() const override;

        private:
            void queue_receive_bytes(std::vector<char> bytes);
            auto drain_receive_buffer(std::span<char> buffer)
                -> coro::task<std::pair<coro::net::io_status, std::span<char>>>;

            auto translate_incoming_message(std::span<const char> message) -> bool;
            auto translate_legacy_connect_request(std::span<const char> message) -> bool;
            auto translate_legacy_request_envelope(std::span<const char> message) -> bool;
            auto translate_legacy_response_envelope(std::span<const char> message) -> bool;

            auto translate_outgoing_frame(const rpc::stream_transport::envelope_prefix& prefix,
                const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>;

            auto translate_connect_response(const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>;
            auto synthesize_addref_receive(const rpc::stream_transport::envelope_prefix& prefix,
                const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>;
            auto translate_call_receive(const rpc::stream_transport::envelope_prefix& prefix,
                const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>;
            auto translate_post_send(const rpc::stream_transport::envelope_prefix& prefix,
                const rpc::stream_transport::envelope_payload& payload) -> std::vector<char>;

            std::shared_ptr<streaming::stream> underlying_;
            std::deque<char> pending_receive_bytes_;
            std::vector<char> raw_receive_buffer_;

            bool handshake_request_seen_ = false;
            std::optional<rpc::stream_transport::envelope_prefix> pending_send_prefix_;
        };
    }
}
