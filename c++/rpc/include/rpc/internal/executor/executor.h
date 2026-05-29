/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// rpc::executor — unified scheduling abstraction used by the streaming stack
// and transports that need background progress. In coroutine builds it aliases
// rpc::coro::scheduler (libcoro or the SGX runtime). In host blocking builds it
// owns a std::thread pool with a work queue. Non-coroutine SGX builds cannot
// expose std::thread, so they use a minimal inline executor shape.

#include <rpc/internal/coroutine_support.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/coro_runtime/runtime.h>

namespace rpc
{
    // Existing coroutine call sites continue to use scheduler operations
    // directly: schedule(), spawn_detached(...), and resume(handle).
    using executor = ::rpc::coro::scheduler;
    using executor_ptr = ::rpc::coro::scheduler_ptr;
}
#elif defined(FOR_SGX)
#  include <chrono>
#  include <cstddef>
#  include <functional>
#  include <memory>

namespace rpc
{
    class executor
    {
    public:
        bool post(std::function<void()> fn)
        {
            if (fn)
            {
                fn();
            }
            return true;
        }

        void schedule() { }
        bool schedule_after(std::chrono::milliseconds) { return true; }
        void shutdown() noexcept { }

        [[nodiscard]] bool is_shutdown() const noexcept { return false; }
        [[nodiscard]] std::size_t worker_count() const noexcept { return 0; }
    };

    using executor_ptr = std::shared_ptr<executor>;
}
#else
#  include <memory>
#  include <rpc/internal/executor/blocking_executor.h>

namespace rpc
{
    // Blocking-mode executor is the std::thread-pool back end. Streaming code
    // calls executor->post(fn) to start a long-running blocking I/O loop,
    // executor->schedule() as a documented no-op (use poll() instead for
    // write-readiness), and executor->schedule_after(d) for timed sleeps that
    // unblock on shutdown.
    using executor = blocking_executor;
    using executor_ptr = std::shared_ptr<executor>;
}
#endif
