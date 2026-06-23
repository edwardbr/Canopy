// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include <streaming/stream.h>
#include <streaming/tcp/namespace.h>
#include <streaming/tcp_blocking/socket.h>

namespace streaming::blocking::tcp
{
    // TCP stream that composes into the streaming transport in both
    // coroutine and blocking builds. The receive/send/close bodies are
    // identical in both modes — mode-specific behaviour lives inside
    // streaming::blocking::tcp::socket (libcoro async I/O vs POSIX poll/recv/send).
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(socket sock);
        ~stream() override;

#ifdef CANOPY_BUILD_COROUTINE
        // Convenience constructor for existing coroutine call sites that
        // hold a libcoro client. Wraps it in a socket internally so the
        // common stream code path stays unchanged.
        stream(
            coro::net::tcp::client&& client,
            std::shared_ptr<rpc::executor> executor);
#endif

        auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> CORO_TASK(::streaming::receive_result) override;

        auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) override;

        [[nodiscard]] bool is_closed() const override;
        void request_close() noexcept override;
        auto set_closed() -> CORO_TASK(void) override;
        [[nodiscard]] peer_info get_peer_info() const override;

    private:
        socket socket_;
        // The streaming transport runs receive_consumer_loop and
        // send_producer_loop on different threads (worker pool in blocking
        // mode, scheduler in coroutine mode) — both touch these flags as
        // they detect/initiate stream closure. Atomic to avoid the data
        // race that surfaces in blocking-mode fan-out tests.
        std::atomic<bool> closed_{false};
        std::atomic<bool> socket_closed_{false};
    };
} // namespace streaming::blocking::tcp
