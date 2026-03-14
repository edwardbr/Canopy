// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

// stream.h - Abstract coroutine-only stream interface (TCP, TLS, WebSocket, SPSC, etc.)
#pragma once

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <canopy/network_config/network_args.h>

namespace streaming
{
    // Abstract stream interface — coroutine-only, protocol-agnostic
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

        // Receive data into buffer
        virtual auto receive(
            rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>>
            = 0;

        // Async send-all: keeps sending until entire span is consumed or error.
        virtual auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> = 0;

        // Check if connection is closed
        virtual bool is_closed() const = 0;

        // Mark connection as closed
        virtual void set_closed() = 0;

        // Return the remote peer's address and port
        virtual peer_info get_peer_info() const = 0;
    };

} // namespace streaming
