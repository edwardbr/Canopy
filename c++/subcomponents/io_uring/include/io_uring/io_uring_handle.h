/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>

#include <io_uring/types.h>
#include <rpc/rpc.h>

namespace rpc::io_uring
{
    class io_uring_handle
    {
    public:
        virtual ~io_uring_handle() = default;

        virtual CORO_TASK(int) get_iouring_data(data& ring_data) = 0;

        // Called after the controller has published one or more SQEs. The
        // handle decides whether that means an SQPOLL wake, an io_uring_enter,
        // or no host action for the current environment.
        virtual CORO_TASK(int) notify_submitted(
            const data& ring_data,
            uint32_t sqe_count) = 0;

        virtual void close() noexcept = 0;
    };
} // namespace rpc::io_uring
