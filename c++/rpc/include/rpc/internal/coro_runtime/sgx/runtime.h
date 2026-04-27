/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/net/io_status.hpp>
#include <coro/sync_wait.hpp>

#include <utility>

namespace rpc::coro::sgx
{
    template<class Awaitable> decltype(auto) sync_wait(Awaitable&& awaitable)
    {
        return ::coro::sync_wait(std::forward<Awaitable>(awaitable));
    }
} // namespace rpc::coro::sgx
