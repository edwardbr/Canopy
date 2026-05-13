/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef FOR_SGX
#  include <rpc/internal/coro_runtime/sgx/mutex.h>
#else
#  include <rpc/internal/coro_runtime/libcoro/mutex.h>
#endif

namespace rpc::coro
{
#ifdef FOR_SGX
    using mutex = sgx::mutex;
    using scoped_lock = sgx::scoped_lock;
#else
    using mutex = libcoro::mutex;
    using scoped_lock = libcoro::scoped_lock;
#endif
} // namespace rpc::coro
