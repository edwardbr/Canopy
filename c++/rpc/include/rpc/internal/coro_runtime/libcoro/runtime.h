/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/coro.hpp>

#include <utility>

namespace rpc::coro::libcoro
{
    using event = ::coro::event;
    using scheduler = ::coro::scheduler;
    using thread_pool = ::coro::thread_pool;

    template<class T> using task = ::coro::task<T>;

    template<class Awaitable> decltype(auto) sync_wait(Awaitable&& awaitable)
    {
        return ::coro::sync_wait(std::forward<Awaitable>(awaitable));
    }
} // namespace rpc::coro::libcoro
