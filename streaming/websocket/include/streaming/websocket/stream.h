// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <wslay/wslay.h>

#include <streaming/stream.h>

namespace streaming::websocket
{
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(std::shared_ptr<::streaming::stream> underlying);
        ~stream();

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;
        stream(stream&&) = delete;
        auto operator=(stream&&) -> stream& = delete;

        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;
        bool is_closed() const override;
        void set_closed() override;
        auto get_peer_info() const -> peer_info override;

        void queue_message(std::vector<uint8_t> data);
        void queue_close(uint16_t code, const std::string& reason);
        bool wants_read() const;
        bool wants_write() const;
        void drain_pending();
        auto do_send() -> coro::task<bool>;

    private:
        static auto send_callback(
            wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int flags, void* user_data) -> ssize_t;
        static auto recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int flags, void* user_data)
            -> ssize_t;
        static void on_msg_recv_callback(
            wslay_event_context_ptr ctx, const wslay_event_on_msg_recv_arg* arg, void* user_data);

        std::shared_ptr<::streaming::stream> underlying_;
        wslay_event_context_ptr wslay_ctx_{nullptr};
        mutable std::mutex wslay_mutex_;
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_messages_;
        size_t current_msg_offset_{0};
        std::queue<std::vector<uint8_t>> pending_outgoing_;
        std::mutex pending_outgoing_mutex_;
        std::vector<uint8_t> outgoing_raw_;
        bool closed_{false};
    };
} // namespace streaming::websocket
