/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <coro/expected.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace rpc::coro::sgx
{
    enum class execution_strategy_t
    {
        process_tasks_on_thread_pool,
        process_tasks_inline
    };

    class scheduler
    {
        struct private_constructor
        {
            explicit private_constructor() = default;
        };

    public:
        class schedule_operation
        {
            friend class scheduler;
            explicit schedule_operation(scheduler& scheduler) noexcept
                : scheduler_(scheduler)
            {
            }

        public:
            auto await_ready() noexcept -> bool { return false; }
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                scheduler_.enqueue_immediate_handle(awaiting_coroutine);
                return true;
            }
            auto await_resume() noexcept -> void { }

        private:
            scheduler& scheduler_;
        };

        class timer_operation
        {
            friend class scheduler;
            explicit timer_operation(
                scheduler& scheduler,
                std::chrono::milliseconds delay) noexcept
                : scheduler_(scheduler)
                , delay_(delay)
            {
            }

        public:
            auto await_ready() const noexcept -> bool { return false; }
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                if (delay_ <= std::chrono::milliseconds{0})
                    scheduler_.enqueue_handle(awaiting_coroutine);
                else
                    scheduler_.enqueue_timer(std::chrono::steady_clock::now() + delay_, awaiting_coroutine);
                return true;
            }
            auto await_resume() const noexcept -> void { }

        private:
            scheduler& scheduler_;
            std::chrono::milliseconds delay_;
        };

        struct options
        {
            explicit options(
                uint32_t thread_count = 1,
                std::function<void()> on_io_thread_start_functor = nullptr,
                std::function<void()> on_io_thread_stop_functor = nullptr,
                thread_pool::options pool = thread_pool::options{},
                execution_strategy_t execution_strategy = execution_strategy_t::process_tasks_on_thread_pool,
                bool schedule_immediate_to_thread_pool = true)
                : thread_count(thread_count)
                , on_io_thread_start_functor(std::move(on_io_thread_start_functor))
                , on_io_thread_stop_functor(std::move(on_io_thread_stop_functor))
                , pool(std::move(pool))
                , execution_strategy(execution_strategy)
                , schedule_immediate_to_thread_pool(schedule_immediate_to_thread_pool)
            {
            }

            uint32_t thread_count;
            std::function<void()> on_io_thread_start_functor;
            std::function<void()> on_io_thread_stop_functor;
            thread_pool::options pool;
            execution_strategy_t execution_strategy;
            bool schedule_immediate_to_thread_pool;
        };

        explicit scheduler(
            options&& opts,
            private_constructor)
            : options_(normalise_options(std::move(opts)))
            , pool_(thread_pool::make_unique(options_.pool))
        {
            timer_handles_.reserve(256);
        }

        static auto make_unique() -> std::unique_ptr<scheduler> { return make_unique(options{}); }
        static auto make_unique(options opts) -> std::unique_ptr<scheduler>
        {
            return std::make_unique<scheduler>(std::move(opts), private_constructor{});
        }

        scheduler(const scheduler&) = delete;
        scheduler(scheduler&&) = delete;
        auto operator=(const scheduler&) -> scheduler& = delete;
        auto operator=(scheduler&&) -> scheduler& = delete;
        ~scheduler() { shutdown(); }

        auto process_events() -> std::size_t
        {
            process_ready_event();
            return size();
        }

        auto process_ready_event() -> bool
        {
            if (execution_lock_.test_and_set(std::memory_order::acquire))
                return false;

            enqueue_expired_timers();
            auto handle = pop_ready_handle();
            if (!handle)
            {
                execution_lock_.clear(std::memory_order::release);
                return false;
            }

            if (options_.execution_strategy == execution_strategy_t::process_tasks_on_thread_pool)
            {
                pool_->resume(handle);
            }
            else
            {
                if (!handle.done())
                    handle.resume();
            }

            execution_lock_.clear(std::memory_order::release);
            return true;
        }

        auto schedule() -> schedule_operation { return schedule_operation{*this}; }

        auto spawn_detached(task<void>&& task_obj) noexcept -> bool
        {
            if (!task_obj.handle())
                return false;

            if (options_.execution_strategy == execution_strategy_t::process_tasks_on_thread_pool)
                return pool_->spawn_detached(std::move(task_obj));

            auto detached_task = detail::make_task_self_deleting(std::move(task_obj));
            std::coroutine_handle<> handle = detached_task.handle();
            lock_queue();
            auto enqueued = enqueue_handle_unlocked(handle);
            unlock_queue();
            if (!enqueued)
                handle.destroy();
            return enqueued;
        }

        auto enqueue_handle(std::coroutine_handle<> handle) noexcept -> void
        {
            if (!handle || handle.done())
                return;
            lock_queue();
            enqueue_handle_unlocked(handle);
            unlock_queue();
        }

        auto spawn_joinable(task<void>&& task_obj) noexcept -> task<void>
        {
            while (task_obj.resume())
            {
            }
            co_return;
        }

        template<typename return_type> [[nodiscard]] auto schedule(task<return_type> task_obj) -> sgx::task<return_type>
        {
            co_await schedule();
            co_return co_await task_obj;
        }

        template<
            typename return_type,
            typename timeout_type>
        [[nodiscard]] auto schedule(
            task<return_type> task_obj,
            timeout_type)
            -> sgx::task<::coro::expected<
                return_type,
                int>>
        {
            if constexpr (std::is_void_v<return_type>)
            {
                co_await schedule(std::move(task_obj));
                co_return ::coro::expected<return_type, int>();
            }
            else
            {
                co_return ::coro::expected<return_type, int>(co_await schedule(std::move(task_obj)));
            }
        }

        auto resume(std::coroutine_handle<> handle) noexcept -> bool
        {
            if (!handle || handle.done())
                return false;
            if (options_.execution_strategy == execution_strategy_t::process_tasks_on_thread_pool)
                return pool_->resume(handle);
            handle.resume();
            return true;
        }

        template<typename range_type> auto resume(const range_type& handles) noexcept -> std::size_t
        {
            std::size_t resumed = 0;
            for (const auto& handle : handles)
            {
                if (resume(handle))
                    ++resumed;
            }
            return resumed;
        }

        [[nodiscard]] auto yield() -> schedule_operation { return schedule(); }

        auto shutdown() noexcept -> void
        {
            shutdown_.store(true, std::memory_order_release);
            pool_->shutdown();
            lock_queue();
            ready_handles_.clear();
            timer_handles_.clear();
            unlock_queue();
        }

        [[nodiscard]] auto is_shutdown() const noexcept { return shutdown_.load(std::memory_order_acquire); }

        [[nodiscard]] auto size() const noexcept -> std::size_t
        {
            lock_queue();
            auto ready = ready_handles_.size();
            auto timers = timer_handles_.size();
            unlock_queue();
            return ready + timers + pool_->size();
        }

        [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

        [[nodiscard]] auto worker_count() const noexcept -> std::size_t { return pool_->thread_count(); }

        auto run_worker(std::size_t worker_index) noexcept -> void { pool_->worker_loop(worker_index); }
        template<
            class StopPredicate,
            class AfterProcess>
        auto run_worker_until(
            std::size_t worker_index,
            StopPredicate&& stop_requested,
            AfterProcess&& after_process) noexcept -> void
        {
            pool_->worker_loop_until(
                worker_index, std::forward<StopPredicate>(stop_requested), std::forward<AfterProcess>(after_process));
        }
        auto process_worker_event() noexcept -> bool { return pool_->process_one(); }

        template<
            typename rep_type,
            typename period_type>
        [[nodiscard]] auto schedule_after(
            std::chrono::duration<
                rep_type,
                period_type> delay) -> task<void>
        {
            co_await timer_operation{*this, std::chrono::duration_cast<std::chrono::milliseconds>(delay)};
        }

        template<
            typename rep_type,
            typename period_type>
        [[nodiscard]] auto yield_for(
            std::chrono::duration<
                rep_type,
                period_type> delay) -> task<void>
        {
            co_await schedule_after(delay);
        }

    private:
        auto enqueue_immediate_handle(std::coroutine_handle<> handle) noexcept -> void
        {
            if (!handle || handle.done())
                return;

            // schedule() is an immediate cooperative yield. The direct-to-pool
            // path avoids a scheduler-queue hop, but can pass work already
            // waiting in the scheduler queue. Keep it configurable so fairness
            // and latency can be measured with the same runtime.
            if (options_.execution_strategy == execution_strategy_t::process_tasks_on_thread_pool
                && options_.schedule_immediate_to_thread_pool)
            {
                pool_->resume(handle);
                return;
            }

            enqueue_handle(handle);
        }

        struct timer_handle
        {
            std::chrono::steady_clock::time_point deadline;
            std::coroutine_handle<> handle;
        };

        static auto normalise_options(options opts) -> options
        {
            if (opts.pool.thread_count == 1 && opts.thread_count != 1)
                opts.pool.thread_count = opts.thread_count;
            else
                opts.thread_count = opts.pool.thread_count;
            return opts;
        }

        static auto pause() noexcept -> void
        {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }

        auto lock_queue() const noexcept -> void
        {
            while (queue_lock_.test_and_set(std::memory_order::acquire))
                pause();
        }

        auto unlock_queue() const noexcept -> void { queue_lock_.clear(std::memory_order::release); }

        auto enqueue_handle_unlocked(std::coroutine_handle<> handle) noexcept -> bool
        {
            if (!handle || handle.done())
                return false;
            for (auto queued : ready_handles_)
            {
                if (queued == handle)
                    return true;
            }
            ready_handles_.push_back(handle);
            return true;
        }

        auto enqueue_timer(
            std::chrono::steady_clock::time_point deadline,
            std::coroutine_handle<> handle) noexcept -> void
        {
            if (!handle || handle.done())
                return;

            lock_queue();
            for (const auto& timer : timer_handles_)
            {
                if (timer.handle == handle)
                {
                    unlock_queue();
                    return;
                }
            }
            timer_handles_.push_back(timer_handle{deadline, handle});
            unlock_queue();
        }

        auto enqueue_expired_timers() noexcept -> void
        {
            auto now = std::chrono::steady_clock::now();

            lock_queue();
            for (auto it = timer_handles_.begin(); it != timer_handles_.end();)
            {
                if (it->deadline <= now)
                {
                    enqueue_handle_unlocked(it->handle);
                    it = timer_handles_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            unlock_queue();
        }

        auto pop_ready_handle() noexcept -> std::coroutine_handle<>
        {
            lock_queue();
            if (ready_handles_.empty())
            {
                unlock_queue();
                return {};
            }

            auto handle = ready_handles_.front();
            ready_handles_.pop_front();
            unlock_queue();
            return handle;
        }

        options options_;
        std::unique_ptr<thread_pool> pool_;
        std::atomic_flag execution_lock_ = ATOMIC_FLAG_INIT;
        mutable std::atomic_flag queue_lock_ = ATOMIC_FLAG_INIT;
        std::deque<std::coroutine_handle<>> ready_handles_;
        std::vector<timer_handle> timer_handles_;
        std::atomic<bool> shutdown_{false};
    };
} // namespace rpc::coro::sgx
