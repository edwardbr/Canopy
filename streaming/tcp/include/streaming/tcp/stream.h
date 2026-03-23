// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <streaming/stream.h>

namespace streaming::tcp
{
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler);

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;

        [[nodiscard]] bool is_closed() const override;
        auto set_closed() -> coro::task<void> override;
        [[nodiscard]] peer_info get_peer_info() const override;
        auto client() -> coro::net::tcp::client&;

    private:
        coro::net::tcp::client client_;
        std::shared_ptr<coro::scheduler> scheduler_;
        bool closed_{false};
        bool socket_closed_{false};
    };
} // namespace streaming::tcp
