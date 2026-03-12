/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <optional>

#include <coro/coro.hpp>
#include <rpc/rpc.h>

#include <streaming/stream.h>

namespace streaming
{
    class stream_acceptor
    {
    public:
        virtual ~stream_acceptor() = default;

        virtual bool init(std::shared_ptr<coro::scheduler> scheduler)
        {
            (void)scheduler;
            return true;
        }

        virtual CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() = 0;
        virtual void stop() = 0;
    };
} // namespace streaming
