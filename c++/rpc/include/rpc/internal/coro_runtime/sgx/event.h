/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <coroutine>
#include <memory>

namespace rpc::coro::sgx
{
    enum class resume_order_policy
    {
        lifo,
        fifo
    };

    class event
    {
    public:
        struct awaiter
        {
            explicit awaiter(const event& e) noexcept
                : event_(e)
            {
            }

            auto await_ready() const noexcept -> bool { return event_.is_set(); }
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
            {
                const void* const set_state = &event_;
                awaiting_coroutine_ = awaiting_coroutine;

                void* old_value = event_.state_.load(std::memory_order::acquire);
                do
                {
                    if (old_value == set_state)
                        return false;

                    next_ = static_cast<awaiter*>(old_value);
                } while (!event_.state_.compare_exchange_weak(
                    old_value, this, std::memory_order::release, std::memory_order::acquire));

                return true;
            }
            auto await_resume() noexcept -> void { }

            awaiter* next_{nullptr};
            std::coroutine_handle<> awaiting_coroutine_{};
            const event& event_;
        };

        explicit event(bool initially_set = false) noexcept
            : state_(initially_set ? static_cast<void*>(this) : nullptr)
        {
        }

        event(const event&) = delete;
        event(event&&) = delete;
        auto operator=(const event&) -> event& = delete;
        auto operator=(event&&) -> event& = delete;

        [[nodiscard]] auto is_set() const noexcept -> bool { return state_.load(std::memory_order::acquire) == this; }

        auto set_scheduler(scheduler* scheduler) noexcept -> void { scheduler_ = scheduler; }

        auto set(resume_order_policy policy = resume_order_policy::lifo) noexcept -> void
        {
            void* old_value = state_.exchange(this, std::memory_order::acq_rel);
            if (old_value == this)
                return;

            if (policy == resume_order_policy::fifo)
                old_value = reverse(static_cast<awaiter*>(old_value));

            auto* waiters = static_cast<awaiter*>(old_value);
            while (waiters)
            {
                auto* next = waiters->next_;
                if (scheduler_)
                    scheduler_->enqueue_handle(waiters->awaiting_coroutine_);
                else
                    waiters->awaiting_coroutine_.resume();
                waiters = next;
            }
        }

        template<class executor_type>
        auto set(
            std::unique_ptr<executor_type>& executor,
            resume_order_policy policy = resume_order_policy::lifo) noexcept -> void
        {
            void* old_value = state_.exchange(this, std::memory_order::acq_rel);
            if (old_value == this)
                return;

            if (policy == resume_order_policy::fifo)
                old_value = reverse(static_cast<awaiter*>(old_value));

            auto* waiters = static_cast<awaiter*>(old_value);
            while (waiters)
            {
                auto* next = waiters->next_;
                executor->resume(waiters->awaiting_coroutine_);
                waiters = next;
            }
        }

        auto operator co_await() const noexcept -> awaiter { return awaiter(*this); }

        auto reset() noexcept -> void
        {
            void* expected = this;
            state_.compare_exchange_strong(expected, nullptr, std::memory_order::acq_rel, std::memory_order::acquire);
        }

    private:
        static auto reverse(awaiter* head) noexcept -> awaiter*
        {
            awaiter* previous = nullptr;
            while (head)
            {
                auto* next = head->next_;
                head->next_ = previous;
                previous = head;
                head = next;
            }
            return previous;
        }

        mutable std::atomic<void*> state_;
        scheduler* scheduler_{nullptr};
    };
} // namespace rpc::coro::sgx
