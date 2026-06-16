// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <streaming/spsc_queue/stream.h>
#include <streaming/stream.h>

namespace streaming::spsc_buffered_stream
{
    // A local buffering adapter for an existing stream.
    //
    // This class does not connect two endpoints and it does not consume a
    // shared queue pair from configuration. It owns private SPSC queues inside
    // one stream object:
    //
    //   caller send()    -> send_q_ -> send_proxy_loop -> underlying->send()
    //   caller receive() <- recv_q_ <- recv_proxy_loop <- underlying->receive()
    //
    // The adapter is useful when a stream stack deliberately wants a queued
    // boundary between the transport-facing stream API and the underlying I/O.
    // For ordinary in-process SPSC transport between two endpoints, use
    // streaming::spsc_queue::stream instead.
    class stream : public ::streaming::stream
    {
    public:
        static auto create(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<coro::scheduler> scheduler) -> std::shared_ptr<stream>;
        ~stream() override;

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;
        bool is_closed() const override;
        auto set_closed() -> coro::task<void> override;
        auto get_peer_info() const -> peer_info override;

    private:
        struct proxy_state
        {
            std::shared_ptr<::streaming::stream> underlying;
            std::shared_ptr<coro::scheduler> scheduler;
            streaming::spsc_queue::queue_type recv_q_;
            streaming::spsc_queue::queue_type send_q_;
            std::atomic<bool> send_failed{false};
            std::atomic<int> send_failure_type{0};
            std::atomic<int> send_failure_native_code{0};
            std::atomic<bool> closed{false};
            std::atomic<bool> close_requested{false};
            std::atomic<size_t> pending_send_blobs{0};
        };

        stream(
            std::shared_ptr<::streaming::stream> underlying,
            std::shared_ptr<coro::scheduler> scheduler);

        void start_proxy_loops();
        void request_stop() const;
        static auto recv_proxy_loop(std::shared_ptr<proxy_state> state) -> coro::task<void>;
        static auto send_proxy_loop(std::shared_ptr<proxy_state> state) -> coro::task<void>;

        std::shared_ptr<proxy_state> state_;
        std::vector<uint8_t> leftover_;
        size_t leftover_offset_{0};
    };
} // namespace streaming::spsc_buffered_stream
