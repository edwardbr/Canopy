// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

// streaming::blocking::tcp::socket — dual-mode socket wrapper consumed by
// streaming::blocking::tcp::stream. The class hides the I/O substrate so the stream
// source code is identical in coroutine and blocking builds.
//
//   Coroutine builds: wraps a libcoro coro::net::tcp::client. read_some()
//     uses libcoro's epoll-driven async read; wait_writable() yields the
//     calling coroutine to the scheduler (matches the previous
//     scheduler->schedule() yield-and-retry behaviour of stream::send).
//
//   Blocking builds: wraps a non-blocking POSIX fd. read_some() does
//     poll(POLLIN) + ::recv; wait_writable() does poll(POLLOUT) so the
//     caller actually waits for kernel buffer space instead of spinning.

#include <chrono>
#include <memory>
#include <utility>

#include <rpc/rpc.h>

#include <streaming/stream.h>
#include <streaming/tcp/namespace.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/net/tcp/client.hpp>
#endif

namespace streaming::blocking::tcp
{
    class socket
    {
    public:
#ifdef CANOPY_BUILD_COROUTINE
        socket(
            coro::net::tcp::client&& client,
            std::shared_ptr<rpc::executor> executor);
#else
        // Takes ownership of a connected file descriptor. The fd is switched
        // to non-blocking mode so EAGAIN on send routes through wait_writable.
        explicit socket(int fd);
#endif

        socket(socket&&) noexcept;
        socket& operator=(socket&&) noexcept;
        socket(const socket&) = delete;
        socket& operator=(const socket&) = delete;
        ~socket();

        // Read up to buffer.size() bytes. timeout == 0 means "use a sensible
        // default" in coroutine mode (delegated to libcoro) and "block
        // indefinitely" in blocking mode (poll with -1).
        auto read_some(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout) -> CORO_TASK(::streaming::receive_result);

        // Park until the socket is writable (or timeout/error). Coroutine mode
        // yields to the scheduler and returns ok immediately — matches the
        // existing coroutine send-loop behaviour of yielding and retrying
        // rather than waiting on epoll. Blocking mode does poll(POLLOUT).
        auto wait_writable(std::chrono::milliseconds timeout) -> CORO_TASK(rpc::io_status);

        [[nodiscard]] int native_handle() const noexcept;
        void shutdown() noexcept;
        void close() noexcept;
        [[nodiscard]] bool is_open() const noexcept;

    private:
#ifdef CANOPY_BUILD_COROUTINE
        coro::net::tcp::client client_;
        std::shared_ptr<rpc::executor> executor_;
#else
        int fd_{-1};
#endif
    };
} // namespace streaming::blocking::tcp
