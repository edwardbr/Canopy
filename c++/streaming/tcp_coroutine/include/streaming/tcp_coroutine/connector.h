/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

#include <io_uring/controller.h>
#include <streaming/tcp/namespace.h>
#include <streaming/tcp_coroutine/stream.h>

namespace streaming::coroutine::tcp
{
    class connector
    {
    public:
        explicit connector(
            std::shared_ptr<rpc::io_uring::controller> controller,
            rpc::executor_ptr executor) noexcept;
        connector(
            std::shared_ptr<rpc::io_uring::controller> controller,
            stream::options stream_options,
            rpc::executor_ptr executor) noexcept;

        connector(const connector&) = delete;
        connector& operator=(const connector&) = delete;
        connector(connector&&) = default;
        connector& operator=(connector&&) = delete;

        CORO_TASK(stream_result)
        connect_loopback(
            uint16_t port,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
        CORO_TASK(stream_result)
        connect_ipv4(
            const std::array<
                uint8_t,
                4>& address,
            uint16_t port,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});
        CORO_TASK(stream_result)
        connect_ipv6(
            const std::array<
                uint8_t,
                16>& address,
            uint16_t port,
            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    private:
        const std::shared_ptr<rpc::io_uring::controller> controller_;
        const stream::options stream_options_;
        const rpc::executor_ptr executor_;
    };

    CORO_TASK(stream_result)
    connect_loopback(
        std::shared_ptr<rpc::io_uring::controller> controller,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout,
        rpc::executor_ptr executor);

    CORO_TASK(stream_result)
    connect_ipv4(
        std::shared_ptr<rpc::io_uring::controller> controller,
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout,
        rpc::executor_ptr executor);

    CORO_TASK(stream_result)
    connect_ipv6(
        std::shared_ptr<rpc::io_uring::controller> controller,
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout,
        rpc::executor_ptr executor);
} // namespace streaming::coroutine::tcp
