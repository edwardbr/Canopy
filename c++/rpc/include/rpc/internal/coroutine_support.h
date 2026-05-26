/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <type_traits>

namespace rpc::compat
{
    template<typename T> using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;
}

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
// Member-style spawn: used as `svc->SPAWN(expr)`. In coroutine builds
// expr is a CORO_TASK(void) (i.e., coro::task<void>) that the scheduler
// runs detached — the macro expands to the existing spawn() call.
#  define SPAWN(expr) spawn(expr)
// Same shape as SPAWN, but for raw rpc::executor (libcoro scheduler in
// coroutine mode, rpc::blocking_executor in blocking mode). Used at call
// sites where you have an executor pointer rather than an rpc::service.
// Coroutine: maps to libcoro's spawn_detached(task<void>&&).
#  define SPAWN_DETACHED(expr) spawn_detached(expr)
#else
#  define CORO_TASK(x) x
#  define CO_RETURN return
#  define CO_AWAIT
#  define SYNC_WAIT(x) x

// Member-style spawn: used as `svc->SPAWN(expr)`. In blocking builds
// expr is a void-returning function call; the macro wraps it in a
// copy-capturing lambda so it runs on a worker thread via the service's
// std::function<void()> spawn overload. Captures are by-value so the
// deferred call sees stable copies of locals and the `this` pointer.
#  define SPAWN(expr) spawn([=]() mutable { (void)(expr); })
// Same shape as SPAWN, but for raw rpc::executor. In blocking mode
// rpc::blocking_executor exposes post(std::function<void()>); the lambda
// wrapper handles the deferred-evaluation semantics.
#  define SPAWN_DETACHED(expr) post([=]() mutable { (void)(expr); })
#endif
