// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include <streaming/stream.h>

namespace streaming::io_uring_tcp
{
    class stream : public ::streaming::stream
    {
    public:
        explicit stream(coro::net::tcp::client&& client, std::shared_ptr<coro::scheduler> scheduler);
        ~stream() override;

        stream(const stream&) = delete;
        auto operator=(const stream&) -> stream& = delete;
        stream(stream&&) = delete;
        auto operator=(stream&&) -> stream& = delete;

        auto receive(rpc::mutable_byte_span buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, rpc::mutable_byte_span>> override;

        auto send(rpc::byte_span buffer) -> coro::task<coro::net::io_status> override;

        [[nodiscard]] bool is_closed() const override;
        void set_closed() override;
        [[nodiscard]] auto get_peer_info() const -> peer_info override;
        auto client() -> coro::net::tcp::client&;

        struct ring_state;

    private:
        static auto completion_pump(std::shared_ptr<ring_state> state, std::shared_ptr<coro::scheduler> scheduler)
            -> coro::task<void>;

        coro::net::tcp::client client_;
        std::shared_ptr<coro::scheduler> scheduler_;
        std::shared_ptr<ring_state> state_;
        bool closed_{false};
        bool socket_closed_{false};
        std::atomic<bool> shutting_down_{false};
    };
} // namespace streaming::io_uring_tcp
