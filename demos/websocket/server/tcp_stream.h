// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// tcp_stream.h - Plain TCP stream wrapper
#pragma once

#include "stream.h"

namespace websocket_demo
{
    namespace v1
    {
        // Plain TCP stream wrapper
        class tcp_stream : public stream
        {
        public:
            explicit tcp_stream(coro::net::tcp::client&& client);

            auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<coro::poll_status> override;

            auto recv(std::span<char> buffer) -> std::pair<coro::net::recv_status, std::span<char>> override;

            auto send(std::span<const char> buffer) -> std::pair<coro::net::send_status, std::span<const char>> override;

            bool is_closed() const override;

            void set_closed() override;

            // Access to underlying client for operations that need it
            coro::net::tcp::client& client();

        private:
            coro::net::tcp::client client_;
            bool closed_{false};
        };

    } // namespace websocket_demo
}
