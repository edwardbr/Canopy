/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include <io_uring/tcp.h>
#include <streaming/tcp/namespace.h>
#include <streaming/tcp_coroutine/stream.h>
#include <streaming/stream_acceptor.h>

namespace streaming::coroutine::tcp
{
    class acceptor : public streaming::stream_acceptor
    {
    public:
        explicit acceptor(
            std::shared_ptr<rpc::io_uring::controller> controller,
            stream::options stream_options = default_stream_options()) noexcept;
        ~acceptor() override = default;

        acceptor(const acceptor&) = delete;
        acceptor& operator=(const acceptor&) = delete;
        acceptor(acceptor&&) = delete;
        acceptor& operator=(acceptor&&) = delete;

        CORO_TASK(int)
        listen_loopback(
            uint16_t port,
            uint32_t backlog = 16);
        CORO_TASK(int)
        listen_ipv4(
            const std::array<
                uint8_t,
                4>& bind_address,
            uint16_t port,
            uint32_t backlog = 16);
        CORO_TASK(int)
        listen_ipv6(
            const std::array<
                uint8_t,
                16>& bind_address,
            uint16_t port,
            uint32_t backlog = 16);

        bool init(std::shared_ptr<rpc::coro::scheduler> scheduler) override;
        CORO_TASK(std::optional<std::shared_ptr<streaming::stream>>) accept() override;
        void stop() override;

        [[nodiscard]] uint16_t port() const noexcept;
        [[nodiscard]] bool is_listening() const noexcept;

    private:
        static CORO_TASK(void) close_acceptor(std::shared_ptr<rpc::io_uring::acceptor> acceptor);

        const std::shared_ptr<rpc::io_uring::controller> controller_;
        std::shared_ptr<rpc::io_uring::acceptor> acceptor_;
        std::shared_ptr<rpc::coro::scheduler> scheduler_;
        const stream::options stream_options_;
        std::atomic<bool> stopping_{false};
    };
} // namespace streaming::coroutine::tcp
