/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace rpc::coro::sgx
{
    class mutex;
    class scoped_lock;

    namespace detail
    {
        struct lock_operation_base
        {
            explicit lock_operation_base(mutex& lock) noexcept
                : lock_(lock)
            {
            }

            lock_operation_base(const lock_operation_base&) = delete;
            lock_operation_base(lock_operation_base&&) = delete;
            auto operator=(const lock_operation_base&) -> lock_operation_base& = delete;
            auto operator=(lock_operation_base&&) -> lock_operation_base& = delete;
            ~lock_operation_base() = default;

            auto await_ready() const noexcept -> bool;
            auto await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool;

            std::coroutine_handle<> awaiting_coroutine_{};
            lock_operation_base* next_{nullptr};

        protected:
            friend class rpc::coro::sgx::mutex;

            mutex& lock_;
        };

        template<typename return_type> struct lock_operation final : public lock_operation_base
        {
            explicit lock_operation(mutex& lock) noexcept
                : lock_operation_base(lock)
            {
            }

            auto await_resume() noexcept -> return_type
            {
                if constexpr (std::is_same_v<scoped_lock, return_type>)
                    return scoped_lock{this->lock_};
                else
                    return;
            }
        };
    } // namespace detail

    class scoped_lock
    {
        friend class mutex;

    public:
        enum class lock_strategy
        {
            adopt
        };

        explicit scoped_lock(
            mutex& lock,
            lock_strategy strategy = lock_strategy::adopt) noexcept
            : lock_(&lock)
        {
            (void)strategy;
        }

        scoped_lock(const scoped_lock&) = delete;
        scoped_lock(scoped_lock&& other) noexcept
            : lock_(
                  std::exchange(
                      other.lock_,
                      nullptr))
        {
        }

        auto operator=(const scoped_lock&) -> scoped_lock& = delete;
        auto operator=(scoped_lock&& other) noexcept -> scoped_lock&
        {
            if (std::addressof(other) != this)
            {
                unlock();
                lock_ = std::exchange(other.lock_, nullptr);
            }
            return *this;
        }

        ~scoped_lock() { unlock(); }

        auto unlock() noexcept -> void;

    private:
        mutex* lock_{nullptr};
    };

    class mutex
    {
    public:
        mutex() noexcept
            : state_(const_cast<void*>(unlocked_value()))
        {
        }

        mutex(const mutex&) = delete;
        mutex(mutex&&) = delete;
        auto operator=(const mutex&) -> mutex& = delete;
        auto operator=(mutex&&) -> mutex& = delete;
        ~mutex() = default;

        [[nodiscard]] auto scoped_lock() noexcept -> detail::lock_operation<sgx::scoped_lock>
        {
            return detail::lock_operation<sgx::scoped_lock>{*this};
        }

        [[nodiscard]] auto lock() noexcept -> detail::lock_operation<void>
        {
            return detail::lock_operation<void>{*this};
        }

        [[nodiscard]] auto try_lock() noexcept -> bool
        {
            void* expected = const_cast<void*>(unlocked_value());
            return state_.compare_exchange_strong(
                expected, nullptr, std::memory_order::acq_rel, std::memory_order::relaxed);
        }

        auto unlock() noexcept -> void
        {
            auto* current = state_.load(std::memory_order::acquire);
            do
            {
                if (current == const_cast<void*>(unlocked_value()))
                    std::terminate();

                if (current == nullptr)
                {
                    if (state_.compare_exchange_weak(
                            current, const_cast<void*>(unlocked_value()), std::memory_order::acq_rel, std::memory_order::acquire))
                    {
                        return;
                    }
                    continue;
                }

                auto* waiter = static_cast<detail::lock_operation_base*>(current);
                if (state_.compare_exchange_weak(
                        current, static_cast<void*>(waiter->next_), std::memory_order::acq_rel, std::memory_order::acquire))
                {
                    waiter->next_ = nullptr;
                    waiter->awaiting_coroutine_.resume();
                    return;
                }
            } while (true);
        }

    private:
        friend struct detail::lock_operation_base;

        auto unlocked_value() const noexcept -> const void* { return &state_; }

        std::atomic<void*> state_;
    };

    inline auto detail::lock_operation_base::await_ready() const noexcept -> bool
    {
        return lock_.try_lock();
    }

    inline auto detail::lock_operation_base::await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept -> bool
    {
        awaiting_coroutine_ = awaiting_coroutine;

        auto& state = lock_.state_;
        auto* current = state.load(std::memory_order::acquire);
        const auto* unlocked = lock_.unlocked_value();
        do
        {
            if (current == unlocked)
            {
                if (state.compare_exchange_weak(current, nullptr, std::memory_order::acq_rel, std::memory_order::acquire))
                {
                    awaiting_coroutine_ = nullptr;
                    return false;
                }
            }
            else
            {
                next_ = static_cast<lock_operation_base*>(current);
                if (state.compare_exchange_weak(
                        current, static_cast<void*>(this), std::memory_order::acq_rel, std::memory_order::acquire))
                {
                    return true;
                }
            }
        } while (true);
    }

    inline auto scoped_lock::unlock() noexcept -> void
    {
        auto* lock = std::exchange(lock_, nullptr);
        if (lock)
            lock->unlock();
    }
} // namespace rpc::coro::sgx
