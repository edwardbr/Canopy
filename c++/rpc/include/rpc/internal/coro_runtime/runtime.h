/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/coro.hpp>

#include <memory>
#include <utility>

namespace rpc::coro
{
    namespace net = ::coro::net;

    using event = ::coro::event;
    using scheduler = ::coro::scheduler;
    using thread_pool = ::coro::thread_pool;

    template<class T> using task = ::coro::task<T>;

    template<class Awaitable> decltype(auto) sync_wait(Awaitable&& awaitable)
    {
        return ::coro::sync_wait(std::forward<Awaitable>(awaitable));
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
