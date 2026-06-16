/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/coro_runtime/libcoro/runtime.h>

#include <memory>
#include <utility>

namespace rpc::coro
{
    namespace net = ::coro::net;

    using event = libcoro::event;
    using scheduler = libcoro::scheduler;
    using thread_pool = libcoro::thread_pool;

    template<class T> using task = libcoro::task<T>;

    template<class Awaitable> decltype(auto) sync_wait(Awaitable&& awaitable)
    {
        return libcoro::sync_wait(std::forward<Awaitable>(awaitable));
    }

    using scheduler_ptr = std::shared_ptr<scheduler>;

    inline auto make_shared_scheduler() -> scheduler_ptr
    {
        return scheduler_ptr(scheduler::make_unique());
    }

    template<class Options> inline auto make_shared_scheduler(Options&& options) -> scheduler_ptr
    {
        return scheduler_ptr(scheduler::make_unique(std::forward<Options>(options)));
    }
} // namespace rpc::coro
