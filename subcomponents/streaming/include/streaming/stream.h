// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// stream.h - Abstract stream interface for TCP and TLS connections
#pragma once

#include <coro/coro.hpp>
#include <span>

#include <canopy/network_config/network_args.h>

namespace streaming
{
    // Abstract stream interface that can be implemented by plain TCP or TLS
    struct peer_info
    {
        canopy::network_config::ip_address addr = {};
        canopy::network_config::ip_address_family family = canopy::network_config::ip_address_family::ipv4;
        uint16_t port = 0;
    };

    class stream
    {
    public:
        virtual ~stream() = default;

        // Poll for read or write readiness
        virtual auto poll(coro::poll_op op, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<coro::poll_status>
            = 0;

        // Receive data into buffer
        virtual auto recv(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>>
            = 0;

        // Send data from buffer (synchronous — must remain non-coroutine for wslay callbacks)
        virtual auto send(std::span<const char> buffer) -> std::pair<coro::net::io_status, std::span<const char>> = 0;

        // Async write-all: keeps writing until entire span is consumed or error.
        // Default implementation loops on send(), yielding via poll() on try_again.
        virtual auto write(std::span<const char> buffer) -> coro::task<coro::net::io_status>;

        // Flush any buffered data to the underlying layer.
        // Default is a no-op; ws_stream overrides to drain wslay output.
        virtual auto flush() -> coro::task<bool> { co_return true; }

        // Check if connection is closed
        virtual bool is_closed() const = 0;

        // Mark connection as closed
        virtual void set_closed() = 0;

        // Return the remote peer's address and port
        virtual peer_info get_peer_info() const = 0;
    };

} // namespace streaming
