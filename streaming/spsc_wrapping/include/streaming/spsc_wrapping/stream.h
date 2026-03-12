// Copyright (c) 2026 Edward Boggis-Rolfe
// All rights reserved.

#pragma once

#include <memory>
#include <vector>

#include <streaming/spsc_queue/stream.h>
#include <streaming/stream.h>

namespace streaming::spsc_wrapping
{
    class stream : public ::streaming::stream, public std::enable_shared_from_this<stream>
    {
    public:
        static auto create(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<coro::scheduler> scheduler)
            -> std::shared_ptr<stream>;

        auto receive(std::span<char> buffer, std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
            -> coro::task<std::pair<coro::net::io_status, std::span<char>>> override;

        auto send(std::span<const char> buffer) -> coro::task<coro::net::io_status> override;
        bool is_closed() const override;
        void set_closed() override;
        auto get_peer_info() const -> peer_info override;

    private:
        stream(std::shared_ptr<::streaming::stream> underlying, std::shared_ptr<coro::scheduler> scheduler);

        void start_proxy_loops();
        static auto recv_proxy_loop(std::shared_ptr<stream> self) -> coro::task<void>;
        static auto send_proxy_loop(std::shared_ptr<stream> self) -> coro::task<void>;

        std::shared_ptr<::streaming::stream> underlying_;
        std::shared_ptr<coro::scheduler> scheduler_;
        streaming::spsc_queue::queue_type recv_q_;
        streaming::spsc_queue::queue_type send_q_;
        std::vector<uint8_t> leftover_;
        bool closed_{false};
    };
} // namespace streaming::spsc_wrapping
