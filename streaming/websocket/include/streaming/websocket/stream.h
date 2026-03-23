// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <wslay/wslay.h>

#include <streaming/stream.h>

namespace streaming::websocket
{
    // Single-executor WebSocket stream.  send() and receive() must not be called
    // concurrently — each belongs to one coroutine at a time.
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(std::shared_ptr<::streaming::stream> underlying);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;
        stream(stream&&) = delete;
        auto operator=(stream&&) -> stream& = delete;

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;
        bool is_closed() const override;
        auto set_closed() -> coro::task<void> override;
        auto get_peer_info() const -> peer_info override;

    private:
        static constexpr size_t io_chunk_size = 8192;

        auto serve_decoded(rpc::mutable_byte_span buffer)
            -> std::pair<coro::net::io_status, rpc::mutable_byte_span>;

        // Drive wslay's outgoing queue until it has nothing left to send.
        auto drive_send() -> coro::task<bool>;
        auto flush_outgoing_raw() -> coro::task<bool>;

        static auto send_callback(
            wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data) -> ssize_t;
        static auto recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data)
            -> ssize_t;
        static void on_msg_recv_callback(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data);

        std::shared_ptr<::streaming::stream> underlying_;
        wslay_event_context_ptr wslay_ctx_{nullptr};
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_messages_;
        size_t current_msg_offset_{0};
        std::vector<uint8_t> outgoing_raw_; // staging buffer populated by send_callback
        bool closed_{false};
    };
} // namespace streaming::websocket
