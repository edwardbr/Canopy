// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <rpc/rpc.h>

#include <canopy/network_config/types.h>

namespace streaming
{
    struct peer_info
    {
        canopy::network_config::ip_address addr = {};
        canopy::network_config::ip_address_family family = canopy::network_config::ip_address_family::ipv4;
        uint16_t port = 0;
    };

    // Compound return type for receive(); the comma inside std::pair would
    // confuse the CORO_TASK macro, so it lives behind a typedef.
    using receive_result = std::pair<rpc::io_status, rpc::mutable_byte_span>;

    class stream
    {
    public:
        virtual ~stream() = default;

        // Receive data into buffer
        virtual auto receive(
            rpc::mutable_byte_span buffer,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) -> CORO_TASK(receive_result) = 0;

        // Async send-all: keeps sending until entire span is consumed or error.
        virtual auto send(rpc::byte_span buffer) -> CORO_TASK(rpc::io_status) = 0;

        // Check if connection is closed
        [[nodiscard]] virtual bool is_closed() const = 0;
        // Non-blocking close request for destructor-driven teardown. The default
        // is a no-op so existing stream adapters remain source-compatible; stream
        // implementations that own asynchronous resources should override it and
        // schedule cleanup on the executor they were constructed with.
        virtual void request_close() noexcept { }
        // Initiate shutdown of the stream and complete only once stream-local shutdown work
        // has reached a stable state for that implementation.
        virtual auto set_closed() -> CORO_TASK(void) = 0;
        [[nodiscard]] virtual peer_info get_peer_info() const = 0;
    };
} // namespace streaming
