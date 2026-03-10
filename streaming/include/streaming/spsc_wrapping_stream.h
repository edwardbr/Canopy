// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// spsc_wrapping_stream.h - Composable SPSC buffering layer that wraps any stream.
//
// Inserts a pair of internal SPSC lock-free queues and proxy loops between the
// transport layer and an underlying stream (e.g. TCP).  The proxy loops run on
// the same scheduler as the transport and decouple the transport's coroutine
// timing from the underlying I/O:
//
//   transport (receive_consumer_loop)
//       ↓ receive() — pops from recv_q_ (timeout if empty)
//   [recv_q_]  ← recv_proxy_loop fills from underlying_->receive()
//
//   [send_q_]  → send_proxy_loop drains to underlying_->send()
//       ↑ send() — pushes to send_q_ and returns immediately
//   transport (send_producer_loop)
//
// Neither receive() nor send() blocks in the underlying stream; that work is
// done by the proxy loops running concurrently on the scheduler.
//
// The proxy loops stop automatically when closed_ is set (via set_closed()).
//
// Usage — create via the factory, never directly:
//   auto spsc = spsc_wrapping_stream::create(tcp_stream, scheduler);
//   auto tls  = std::make_shared<tls_stream>(spsc, tls_ctx);
//   streaming_transport::create("name", service, tls, handler);
#pragma once

#include <memory>
#include <vector>

#include "stream.h"
#include "spsc_queue_stream.h"

namespace streaming
{
    // Wraps any underlying stream through a pair of internal SPSC queues with
    // dedicated proxy coroutines bridging the queue ↔ underlying stream boundary.
    class spsc_wrapping_stream : public stream, public std::enable_shared_from_this<spsc_wrapping_stream>
    {
    public:
        // Factory — creates the object and spawns the proxy loops on scheduler.
        // Must be used instead of direct construction.
        static std::shared_ptr<spsc_wrapping_stream> create(
            std::shared_ptr<stream> underlying, std::shared_ptr<coro::scheduler> scheduler);

        // Stream interface — never blocks in the underlying stream directly.
        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;

        bool is_closed() const override;
        void set_closed() override;
        peer_info get_peer_info() const override;

    private:
        spsc_wrapping_stream(std::shared_ptr<stream> underlying, std::shared_ptr<coro::scheduler> scheduler);

        void start_proxy_loops();

        // Fills recv_q_ by reading from underlying_ in a loop.
        static coro::task<void> recv_proxy_loop(std::shared_ptr<spsc_wrapping_stream> self);

        // Drains send_q_ to underlying_ in a loop.
        static coro::task<void> send_proxy_loop(std::shared_ptr<spsc_wrapping_stream> self);

        std::shared_ptr<stream> underlying_;
        std::shared_ptr<coro::scheduler> scheduler_;

        spsc_raw_queue recv_q_;
        spsc_raw_queue send_q_;

        // Overflow bytes from a partially consumed receive blob
        std::vector<uint8_t> leftover_;
        bool closed_{false};
    };

} // namespace streaming
