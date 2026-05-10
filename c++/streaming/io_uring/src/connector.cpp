/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <streaming/io_uring/connector.h>

#include <io_uring/tcp.h>

#include <utility>

namespace streaming::io_uring
{
    connector::connector(
        std::shared_ptr<rpc::io_uring::controller> controller,
        stream::options stream_options) noexcept
        : controller_(std::move(controller))
        , stream_options_(stream_options)
    {
    }

    CORO_TASK(stream_result) connector::connect_loopback(uint16_t port)
    {
        rpc::io_uring::connector low_level_connector(controller_);
        auto result = CO_AWAIT low_level_connector.connect_loopback_with_result(port);
        CO_RETURN make_stream_result(result, port, stream_options_);
    }

    CORO_TASK(stream_result)
    connect_loopback(
        std::shared_ptr<rpc::io_uring::controller> controller,
        uint16_t port,
        stream::options stream_options)
    {
        connector connection_factory(std::move(controller), stream_options);
        CO_RETURN CO_AWAIT connection_factory.connect_loopback(port);
    }
} // namespace streaming::io_uring
