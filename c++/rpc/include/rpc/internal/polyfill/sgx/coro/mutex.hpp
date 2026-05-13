/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/coro_runtime/sgx/mutex.h>

namespace coro
{
    using mutex = ::rpc::coro::sgx::mutex;
    using scoped_lock = ::rpc::coro::sgx::scoped_lock;
} // namespace coro
