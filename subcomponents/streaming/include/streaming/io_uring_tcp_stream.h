// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// io_uring_tcp_stream.h - Linux io_uring-backed TCP stream wrapper
#pragma once

#include "tcp_stream.h"

#if defined(__linux__)

#include <memory>

namespace streaming
{
    class io_uring_tcp_stream : public stream
    {
    public:
        explicit io_uring_tcp_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler);
        ~io_uring_tcp_stream() override;

        io_uring_tcp_stream(const io_uring_tcp_stream&) = delete;
        auto operator=(const io_uring_tcp_stream&) -> io_uring_tcp_stream& = delete;
        io_uring_tcp_stream(io_uring_tcp_stream&&) = delete;
        auto operator=(io_uring_tcp_stream&&) -> io_uring_tcp_stream& = delete;

        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;

        bool is_closed() const override;
        void set_closed() override;
        peer_info get_peer_info() const override;

        coro::net::tcp::client& client();

        struct ring_state;

    private:
        // Internal readiness polling used by receive() and send(); not part of the stream interface.
        auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<coro::poll_status>;

        static auto completion_pump(std::shared_ptr<ring_state> state, std::shared_ptr<coro::scheduler> scheduler)
            -> coro::task<void>;

        coro::net::tcp::client client_;
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<ring_state> state_;
        bool closed_{false};
        bool socket_closed_{false};
    };

} // namespace streaming

#else

namespace streaming
{
    using io_uring_tcp_stream = tcp_stream;
}

#endif
