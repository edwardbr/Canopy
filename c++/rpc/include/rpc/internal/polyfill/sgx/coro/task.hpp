/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/internal/coro_runtime/sgx/task.h>

namespace coro
{
    template<typename return_type = void> using task = ::rpc::coro::sgx::task<return_type>;
} // namespace coro
