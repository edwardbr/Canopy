// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// stream.h - Abstract stream interface for TCP and TLS connections
#pragma once

#include <coro/coro.hpp>
#include <span>

namespace websocket_demo
{
    namespace v1
    {
        // Abstract stream interface that can be implemented by plain TCP or TLS
        class stream
        {
        public:
            virtual ~stream() = default;

            // Poll for read or write readiness
            virtual auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
                -> coro::task<coro::poll_status>
                = 0;

            // Receive data into buffer
            virtual auto recv(std::span<char> buffer) -> std::pair<coro::net::recv_status, std::span<char>> = 0;

            // Send data from buffer
            virtual auto send(std::span<const char> buffer) -> std::pair<coro::net::send_status, std::span<const char>> = 0;

            // Check if connection is closed
            virtual bool is_closed() const = 0;

            // Mark connection as closed
            virtual void set_closed() = 0;
        };

    } // namespace websocket_demo
}
