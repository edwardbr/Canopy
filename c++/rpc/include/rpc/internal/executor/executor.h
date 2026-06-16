/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// rpc::executor — unified scheduling abstraction used by the streaming stack
// and transports that need background progress. In coroutine builds it aliases
// rpc::coro::scheduler. In blocking builds it owns a std::thread pool with a
// work queue.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <rpc/internal/coroutine_support.h>
#include <rpc/internal/polyfill/event.h>
#include <rpc/rpc_types.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <rpc/internal/coro_runtime/runtime.h>
#  include <thread>

namespace rpc
{
    // Existing coroutine call sites continue to use scheduler operations
    // directly: schedule(), spawn_detached(...), and resume(handle).
    using executor = ::rpc::coro::scheduler;
}
#else
#  include <rpc/internal/executor/blocking_executor.h>

namespace rpc
{
    // Blocking-mode executor is the std::thread-pool back end. Streaming code
    // calls executor->post(fn) to start a long-running blocking I/O loop,
    // executor->schedule() as a documented no-op (use poll() instead for
    // write-readiness), and executor->schedule_after(d) for timed sleeps that
    // unblock on shutdown.
    using executor = blocking_executor;
}
#endif

namespace rpc
{
    using executor_ptr = std::shared_ptr<executor>;
}

#ifdef CANOPY_BUILD_COROUTINE
namespace rpc
{
    inline executor_ptr make_executor(executor_options options = {})
    {
        auto scheduler_options = rpc::coro::scheduler::options{};
        scheduler_options.thread_strategy = rpc::coro::scheduler::thread_strategy_t::spawn;
        scheduler_options.pool.thread_count
            = std::max<uint32_t>(options.thread_count, std::max<uint32_t>(1U, std::thread::hardware_concurrency()));
        scheduler_options.execution_strategy = rpc::coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool;
        return rpc::coro::make_shared_scheduler(scheduler_options);
    }
}
#else
namespace rpc
{
    inline executor_ptr make_executor(executor_options options = {})
    {
        rpc::blocking_executor::options executor_options;
        executor_options.thread_count = options.thread_count;
        executor_options.max_thread_count = options.max_thread_count;
        executor_options.enable_adaptive_threads = options.enable_adaptive_threads;
        return std::make_shared<executor>(executor_options);
    }
}
#endif

namespace rpc
{
    template<class Callable> struct executor_operation
    {
        executor_ptr executor;
        Callable callable;
    };

    template<class Callable>
    auto on_executor(
        executor_ptr executor,
        Callable&& callable) -> executor_operation<std::decay_t<Callable>>
    {
        return {std::move(executor), std::forward<Callable>(callable)};
    }

    template<class... Operations> struct executor_when_all
    {
        std::tuple<Operations...> operations;

        explicit executor_when_all(Operations&&... input)
            : operations(std::move(input)...)
        {
        }
    };

    template<class... Operations>
    auto when_all(Operations&&... operations) -> executor_when_all<std::decay_t<Operations>...>
    {
        return executor_when_all<std::decay_t<Operations>...>{std::forward<Operations>(operations)...};
    }

    namespace detail
    {
        struct executor_when_all_state
        {
            explicit executor_when_all_state(std::size_t count)
                : remaining(count)
            {
                if (count == 0)
                    done.set();
            }

            rpc::event done;
            std::atomic<std::size_t> remaining;
            std::atomic_bool ok{true};

            void fail() { ok.store(false, std::memory_order_release); }

            void complete_one()
            {
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    done.set();
            }
        };

        template<class Operation> bool can_schedule_operation(const Operation& operation)
        {
            return operation.executor && !operation.executor->is_shutdown();
        }

        template<class Operation>
        CORO_TASK(void)
        run_operation(
            Operation& operation,
            const std::shared_ptr<executor_when_all_state>& state)
        {
            try
            {
                CO_AWAIT operation.callable();
            }
            catch (...)
            {
                state->fail();
            }
            state->complete_one();
            CO_RETURN;
        }

        template<class Operation>
        void schedule_operation(
            Operation& operation,
            const std::shared_ptr<executor_when_all_state>& state)
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto submitted = operation.executor->spawn_detached(run_operation(operation, state));
#else
            auto submitted = operation.executor->post([&operation, state] { run_operation(operation, state); });
#endif
            if (!submitted)
            {
                state->fail();
                state->complete_one();
            }
        }
    }

    template<class... Operations> bool sync_wait(executor_when_all<Operations...>&& work)
    {
        constexpr auto operation_count = sizeof...(Operations);
        auto state = std::make_shared<detail::executor_when_all_state>(operation_count);
        auto can_schedule = std::apply(
            [](auto&... operation) { return (detail::can_schedule_operation(operation) && ...); }, work.operations);
        if (!can_schedule)
            return false;

        std::apply([&state](auto&... operation) { (detail::schedule_operation(operation, state), ...); }, work.operations);
        SYNC_WAIT(state->done.wait());
        return state->ok.load(std::memory_order_acquire);
    }

    inline void shutdown_executor(const executor_ptr& executor)
    {
        if (executor)
            executor->shutdown();
    }
}
