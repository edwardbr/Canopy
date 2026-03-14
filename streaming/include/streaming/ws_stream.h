// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// ws_stream.h - WebSocket framing stream
#pragma once

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <wslay/wslay.h>

#include "stream.h"

namespace streaming
{
    // WebSocket framing stream that wraps any stream using wslay.
    // receive() returns the payload of one complete WebSocket binary message.
    // send() enqueues a binary frame and flushes it to the underlying stream.
    class ws_stream : public stream
    {
    public:
        explicit ws_stream(std::shared_ptr<stream> underlying);
        ~ws_stream();

        // Non-copyable and non-movable (wslay context holds a pointer to this)
        ws_stream(const ws_stream&) = delete;
        ws_stream& operator=(const ws_stream&) = delete;
        ws_stream(ws_stream&&) = delete;
        ws_stream& operator=(ws_stream&&) = delete;

        // stream interface
        // Returns the payload of the next complete WebSocket binary message.
        // Partial reads are supported: if the message is larger than buffer,
        // subsequent calls return the remaining bytes of the same message.
        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;

        bool is_closed() const override;
        void set_closed() override;
        peer_info get_peer_info() const override;

        // Thread-safe: queue a binary message for sending (may be called from transport threads)
        void queue_message(std::vector<uint8_t> data);

        // Queue a WebSocket close frame
        void queue_close(uint16_t code, const std::string& reason);

        bool wants_read() const;
        bool wants_write() const;

        // Move queued outgoing messages into wslay (call before do_send)
        void drain_pending();

        // Flush wslay output to the underlying stream
        auto do_send() -> coro::task<bool>;

    private:
        static ssize_t send_callback(
            wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data);

        static ssize_t recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data);

        static void on_msg_recv_callback(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data);

        std::shared_ptr<stream> underlying_;
        wslay_event_context_ptr wslay_ctx_{nullptr};
        mutable std::mutex wslay_mutex_;

        // Raw receive buffer for feeding the wslay recv_callback
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0}; // valid bytes in raw_recv_buffer_
        size_t raw_recv_pos_{0};

        // Complete decoded WebSocket messages awaiting consumption
        std::queue<std::vector<uint8_t>> decoded_messages_;
        size_t current_msg_offset_{0}; // read position within decoded_messages_.front()

        // Outgoing message queue — written by transport threads, drained by drain_pending()
        std::queue<std::vector<uint8_t>> pending_outgoing_;
        std::mutex pending_outgoing_mutex_;

        // Staging buffer: send_callback writes here; do_send() drains to underlying_->send()
        std::vector<uint8_t> outgoing_raw_;

        bool closed_{false};
    };

} // namespace streaming
