/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <memory>
#include <optional>

#include <rpc/rpc.h>

#include <streaming/stream.h>

namespace streaming
{
    class stream_acceptor
    {
    public:
        virtual ~stream_acceptor() = default;

        // init(executor) replaces init(scheduler). In coroutine builds the
        // type is identical (rpc::executor is an alias of rpc::coro::scheduler);
        // in blocking builds the executor wraps a std::thread pool. Concrete
        // acceptors that need to interrupt a blocked syscall should implement
        // that in stop(); the executor is the progress engine, not a hook
        // registry.
        virtual bool init(std::shared_ptr<rpc::executor> executor)
        {
            (void)executor;
            return true;
        }

        virtual CORO_TASK(std::optional<std::shared_ptr<stream>>) accept() = 0;
        virtual void stop() = 0;
    };
} // namespace streaming
