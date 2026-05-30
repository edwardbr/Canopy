// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <sys/types.h>
#include <vector>

#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/coro_runtime/mutex.h>
#endif
#include <streaming/stream.h>

struct wslay_event_context;
struct wslay_event_on_msg_recv_arg;

namespace streaming::websocket
{
    enum class stream_role
    {
        client,
        server
    };

    struct keep_alive_options
    {
        bool enabled{false};
        std::chrono::milliseconds interval{std::chrono::milliseconds{30000}};
        std::chrono::milliseconds timeout{std::chrono::milliseconds{10000}};
    };

    struct stream_options
    {
        stream_role role{stream_role::server};
        keep_alive_options keep_alive;
    };

    // WebSocket stream over any streaming::stream. wslay's context is not
    // thread-safe, so this wrapper serializes access to wslay-owned state while
    // leaving underlying stream I/O outside the lock.
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(std::shared_ptr<::streaming::stream> underlying);
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            stream_role role);
        stream(
            std::shared_ptr<::streaming::stream> underlying,
            stream_options options);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;
        stream(stream&&) = delete;
        auto operator=(stream&&) -> stream& = delete;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override;

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override;
        bool is_closed() const override;
        auto set_closed() -> CORO_TASK(void) override;
        auto get_peer_info() const -> peer_info override;

    private:
        static constexpr size_t io_chunk_size = 8192;

        auto serve_decoded(rpc::mutable_byte_span buffer) -> std::pair<
            rpc::io_status,
            rpc::mutable_byte_span>;
        auto serve_decoded_locked(rpc::mutable_byte_span buffer) -> std::pair<
            rpc::io_status,
            rpc::mutable_byte_span>;

        // Drive wslay's outgoing queue until it has nothing left to send.
        auto drive_send() -> CORO_TASK(bool);
        auto drive_send_locked() -> CORO_TASK(bool);
        auto flush_outgoing_raw(std::vector<uint8_t> raw) -> CORO_TASK(bool);
        auto maybe_queue_keep_alive_locked(std::chrono::steady_clock::time_point now) -> bool;
        auto next_receive_timeout_locked(
            std::chrono::steady_clock::time_point deadline,
            bool single_attempt,
            std::chrono::steady_clock::time_point now) const -> std::chrono::milliseconds;
        void handle_keep_alive_locked(const wslay_event_on_msg_recv_arg* arg);

        static auto send_callback(
            wslay_event_context* ctx,
            const uint8_t* data,
            size_t len,
            int flags,
            void* user_data) -> ssize_t;
        static auto recv_callback(
            wslay_event_context* ctx,
            uint8_t* buf,
            size_t len,
            int flags,
            void* user_data) -> ssize_t;
        static auto genmask_callback(
            wslay_event_context* ctx,
            uint8_t* buf,
            size_t len,
            void* user_data) -> int;
        static void on_msg_recv_callback(
            wslay_event_context* ctx,
            const wslay_event_on_msg_recv_arg* arg,
            void* user_data);

        mutable std::mutex mtx_;
#ifdef CANOPY_BUILD_COROUTINE
        rpc::coro::mutex send_mtx_;
#else
        std::mutex send_mtx_;
#endif
        std::shared_ptr<::streaming::stream> underlying_;
        wslay_event_context* wslay_ctx_{nullptr};
        stream_options options_;
        std::string raw_recv_buffer_;
        size_t raw_recv_size_{0};
        size_t raw_recv_pos_{0};
        std::queue<std::vector<uint8_t>> decoded_messages_;
        size_t current_msg_offset_{0};
        std::vector<uint8_t> outgoing_raw_; // staging buffer populated by send_callback
        std::vector<uint8_t> pending_ping_payload_;
        uint64_t next_ping_id_{1};
        std::chrono::steady_clock::time_point next_ping_time_;
        std::chrono::steady_clock::time_point ping_deadline_;
        bool ping_outstanding_{false};
        bool closed_{false};
    };
} // namespace streaming::websocket
