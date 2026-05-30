/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/tcp_coroutine/connector.h>

#include <io_uring/tcp.h>

#include <utility>

namespace streaming::coroutine::tcp
{
    connector::connector(
        std::shared_ptr<rpc::io_uring::controller> controller,
        stream::options stream_options) noexcept
        : controller_(std::move(controller))
        , stream_options_(stream_options)
    {
    }

    CORO_TASK(stream_result)
    connector::connect_loopback(
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        CO_RETURN CO_AWAIT connect_ipv4({127, 0, 0, 1}, port, timeout);
    }

    CORO_TASK(stream_result)
    connector::connect_ipv4(
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        rpc::io_uring::connector low_level_connector(controller_);
        auto result = CO_AWAIT low_level_connector.connect_ipv4_with_result(address, port, timeout);
        CO_RETURN make_stream_result(result, port, stream_options_);
    }

    CORO_TASK(stream_result)
    connector::connect_ipv6(
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port,
        std::chrono::milliseconds timeout)
    {
        rpc::io_uring::connector low_level_connector(controller_);
        auto result = CO_AWAIT low_level_connector.connect_ipv6_with_result(address, port, timeout);
        CO_RETURN make_stream_result(result, port, stream_options_);
    }

    CORO_TASK(stream_result)
    connect_loopback(
        std::shared_ptr<rpc::io_uring::controller> controller,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout)
    {
        connector connection_factory(std::move(controller), stream_options);
        CO_RETURN CO_AWAIT connection_factory.connect_loopback(port, timeout);
    }

    CORO_TASK(stream_result)
    connect_ipv4(
        std::shared_ptr<rpc::io_uring::controller> controller,
        const std::array<
            uint8_t,
            4>& address,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout)
    {
        connector connection_factory(std::move(controller), stream_options);
        CO_RETURN CO_AWAIT connection_factory.connect_ipv4(address, port, timeout);
    }

    CORO_TASK(stream_result)
    connect_ipv6(
        std::shared_ptr<rpc::io_uring::controller> controller,
        const std::array<
            uint8_t,
            16>& address,
        uint16_t port,
        stream::options stream_options,
        std::chrono::milliseconds timeout)
    {
        connector connection_factory(std::move(controller), stream_options);
        CO_RETURN CO_AWAIT connection_factory.connect_ipv6(address, port, timeout);
    }
} // namespace streaming::coroutine::tcp
