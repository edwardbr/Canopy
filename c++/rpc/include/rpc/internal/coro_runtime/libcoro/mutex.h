/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/mutex.hpp>

namespace rpc::coro::libcoro
{
    using mutex = ::coro::mutex;
    using scoped_lock = ::coro::scoped_lock;
} // namespace rpc::coro::libcoro
