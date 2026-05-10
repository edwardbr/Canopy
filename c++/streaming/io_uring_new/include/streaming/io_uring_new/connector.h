/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <memory>

#include <io_uring/controller.h>
#include <streaming/io_uring_new/stream.h>

namespace streaming::io_uring_new
{
    class connector
    {
    public:
        explicit connector(
            std::shared_ptr<rpc::io_uring::controller> controller,
            stream::options stream_options = default_stream_options()) noexcept;

        connector(const connector&) = delete;
        connector& operator=(const connector&) = delete;
        connector(connector&&) = default;
        connector& operator=(connector&&) = default;

        CORO_TASK(stream_result) connect_loopback(uint16_t port);

    private:
        std::shared_ptr<rpc::io_uring::controller> controller_;
        stream::options stream_options_;
    };

    CORO_TASK(stream_result)
    connect_loopback(
        std::shared_ptr<rpc::io_uring::controller> controller,
        uint16_t port,
        stream::options stream_options = default_stream_options());
} // namespace streaming::io_uring_new
