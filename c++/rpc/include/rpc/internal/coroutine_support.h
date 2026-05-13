/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE
#  include <coroutine>
#  ifdef FOR_SGX
#    include <rpc/internal/coro_runtime/sgx/task.h>
#    include <rpc/internal/coro_runtime/sgx/thread_pool.h>
#    include <rpc/internal/coro_runtime/sgx/scheduler.h>
#    include <rpc/internal/coro_runtime/sgx/event.h>
#    include <rpc/internal/coro_runtime/sgx/runtime.h>
#  else
#    include <rpc/internal/coro_runtime/libcoro/runtime.h>
#  endif
#  include <rpc/internal/coro_runtime/runtime.h>
#  include <rpc/internal/coro_runtime/mutex.h>
#  define CORO_TASK(x) ::rpc::coro::task<x>
#  define CO_RETURN co_return
#  define CO_AWAIT co_await
#  define SYNC_WAIT(x) ::rpc::coro::sync_wait(x)
#else
#  define CORO_TASK(x) x
#  define CO_RETURN return
#  define CO_AWAIT
#  define SYNC_WAIT(x) x
#endif
