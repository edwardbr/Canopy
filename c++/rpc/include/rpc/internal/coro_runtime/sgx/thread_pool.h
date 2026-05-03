/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace rpc::coro::sgx
{
    class thread_pool
    {
        struct private_constructor
        {
            explicit private_constructor() = default;
        };

    public:
        class schedule_operation
        {
            friend class thread_pool;
            explicit schedule_operation(thread_pool& pool) noexcept
                : pool_(pool)
            {
            }

        public:
            auto await_ready() noexcept -> bool { return false; }
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> void
            {
                pool_.resume(awaiting_coroutine);
            }
            auto await_resume() noexcept -> void { }

        private:
            thread_pool& pool_;
        };

        struct options
        {
            explicit options(
                uint32_t thread_count = 1,
                std::function<void(std::size_t)> on_thread_start_functor = nullptr,
                std::function<void(std::size_t)> on_thread_stop_functor = nullptr)
                : thread_count(thread_count)
                , on_thread_start_functor(std::move(on_thread_start_functor))
                , on_thread_stop_functor(std::move(on_thread_stop_functor))
            {
            }

            uint32_t thread_count;
            std::function<void(std::size_t)> on_thread_start_functor;
            std::function<void(std::size_t)> on_thread_stop_functor;
        };

        explicit thread_pool(
            options&& opts,
            private_constructor)
            : options_(std::move(opts))
        {
            ready_handles_.reserve(256);
        }

        static auto make_unique() -> std::unique_ptr<thread_pool> { return make_unique(options{}); }

        static auto make_unique(options opts) -> std::unique_ptr<thread_pool>
        {
            return std::make_unique<thread_pool>(std::move(opts), private_constructor{});
        }

        thread_pool(const thread_pool&) = delete;
        thread_pool(thread_pool&&) = delete;
        auto operator=(const thread_pool&) -> thread_pool& = delete;
        auto operator=(thread_pool&&) -> thread_pool& = delete;
        ~thread_pool() { shutdown(); }

        [[nodiscard]] auto thread_count() const noexcept -> std::size_t { return options_.thread_count; }

        auto schedule() -> schedule_operation { return schedule_operation{*this}; }

        auto spawn_detached(task<void>&& task_obj) noexcept -> bool
        {
            if (!task_obj.handle())
                return false;

            auto detached_task = detail::make_task_self_deleting(std::move(task_obj));
            std::coroutine_handle<> handle = detached_task.handle();
            if (resume(handle))
                return true;

            handle.destroy();
            return false;
        }

        auto spawn_joinable(task<void>&& task_obj) noexcept -> task<void>
        {
            while (task_obj.resume())
            {
            }
            co_return;
        }

        auto resume(std::coroutine_handle<> handle) noexcept -> bool
        {
            if (!handle || handle.done() || is_shutdown())
                return false;

            lock_queue();
            auto enqueued = enqueue_handle_unlocked(handle);
            unlock_queue();
            return enqueued;
        }

        template<typename range_type> auto resume(const range_type& handles) noexcept -> std::size_t
        {
            std::size_t enqueued = 0;
            for (const auto& handle : handles)
            {
                if (resume(handle))
                    ++enqueued;
            }
            return enqueued;
        }

        [[nodiscard]] auto yield() -> schedule_operation { return schedule(); }

        auto shutdown() noexcept -> void { shutdown_.store(true, std::memory_order_release); }
        [[nodiscard]] auto is_shutdown() const noexcept -> bool { return shutdown_.load(std::memory_order_acquire); }

        [[nodiscard]] auto size() const noexcept -> std::size_t
        {
            lock_queue();
            auto size = ready_handles_.size();
            unlock_queue();
            return size;
        }

        [[nodiscard]] auto empty() const noexcept -> bool { return size() == 0; }

        auto process_one() noexcept -> bool
        {
            auto handle = pop_ready_handle();
            if (!handle)
                return false;

            if (!handle.done())
                handle.resume();
            return true;
        }

        auto worker_loop(std::size_t worker_index) noexcept -> void
        {
            if (options_.on_thread_start_functor)
                options_.on_thread_start_functor(worker_index);

            while (!is_shutdown())
            {
                if (!process_one())
                    pause();
            }

            while (process_one())
            {
            }

            if (options_.on_thread_stop_functor)
                options_.on_thread_stop_functor(worker_index);
        }

        template<
            class StopPredicate,
            class AfterProcess>
        auto worker_loop_until(
            std::size_t worker_index,
            StopPredicate&& stop_requested,
            AfterProcess&& after_process) noexcept -> void
        {
            if (options_.on_thread_start_functor)
                options_.on_thread_start_functor(worker_index);

            while (!is_shutdown() && !stop_requested())
            {
                auto processed = process_one();
                after_process(processed);
                if (!processed)
                    pause();
            }

            while (process_one())
                after_process(true);

            if (options_.on_thread_stop_functor)
                options_.on_thread_stop_functor(worker_index);
        }

    private:
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

        auto pop_ready_handle() noexcept -> std::coroutine_handle<>
        {
            lock_queue();
            if (ready_handles_.empty())
            {
                unlock_queue();
                return {};
            }

            auto handle = ready_handles_.front();
            ready_handles_.erase(ready_handles_.begin());
            unlock_queue();
            return handle;
        }

        options options_;
        mutable std::atomic_flag queue_lock_ = ATOMIC_FLAG_INIT;
        std::vector<std::coroutine_handle<>> ready_handles_;
        std::atomic<bool> shutdown_{false};
    };
} // namespace rpc::coro::sgx
