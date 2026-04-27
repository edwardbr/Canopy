// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <rpc/rpc.h>
#include <coro/task.hpp>
#include <coro/net/io_status.hpp>

#ifndef FOR_SGX
#  include <canopy/network_config/network_args.h>
#endif

namespace streaming
{
    struct peer_info
    {
#ifndef FOR_SGX
        canopy::network_config::ip_address addr = {};
        canopy::network_config::ip_address_family family = canopy::network_config::ip_address_family::ipv4;
#endif
        uint16_t port = 0;
    };

    class stream
    {
    public:
        virtual ~stream() = default;

        // Receive data into buffer
        virtual auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<
                coro::net::io_status,
                rpc::mutable_byte_span>> = 0;

        // Async send-all: keeps sending until entire span is consumed or error.
        virtual auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> = 0;

        // Check if connection is closed
        [[nodiscard]] virtual bool is_closed() const = 0;
        // Initiate shutdown of the stream and complete only once stream-local shutdown work
        // has reached a stable state for that implementation.
        virtual auto set_closed() -> coro::task<void> = 0;
        [[nodiscard]] virtual peer_info get_peer_info() const = 0;
    };
} // namespace streaming
