/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE
#  include <coroutine>
#  include <rpc/internal/coro_runtime/runtime.h>
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
