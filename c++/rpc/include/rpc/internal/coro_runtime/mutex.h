/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/coro_runtime/libcoro/mutex.h>

namespace rpc::coro
{
    using mutex = libcoro::mutex;
    using scoped_lock = libcoro::scoped_lock;
} // namespace rpc::coro
