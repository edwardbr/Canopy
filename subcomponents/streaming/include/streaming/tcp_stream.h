// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.h - Plain TCP stream wrapper
#pragma once

#include "stream.h"

namespace streaming
{
    // Plain TCP stream wrapper
    class tcp_stream : public stream
    {
    public:
        explicit tcp_stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler);

        auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<coro::poll_status> override;

        auto recv(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>> override;

        bool is_closed() const override;

        void set_closed() override;

        peer_info get_peer_info() const override;

        // Access to underlying client for operations that need it
        coro::net::tcp::client& client();

    private:
        coro::net::tcp::client client_;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool closed_{false};
    };

} // namespace streaming
