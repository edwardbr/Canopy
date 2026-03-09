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
    // Abstract base: produces one stream per "connection".
    // Returns nullopt when no more connections will come (stopped, error, or single-use exhausted).
    class stream_acceptor
    {
    public:
        virtual ~stream_acceptor() = default;

        // Called once before the first accept(), with the scheduler from the owning service.
        // Default is a no-op for acceptors that receive their scheduler at construction time.
        virtual bool init(std::shared_ptr<coro::scheduler> scheduler)
        {
            (void)scheduler;
            return true;
        }

        virtual CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() = 0;
        virtual void stop() = 0;
    };

} // namespace streaming
