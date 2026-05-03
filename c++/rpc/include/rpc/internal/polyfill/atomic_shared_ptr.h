/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace rpc
{
    template<typename T> class shared_ptr;
    template<typename T> class optimistic_ptr;
}

namespace rpc::polyfill
{
    template<typename pointer_type>
    inline bool atomic_pointer_equal(
        const pointer_type& lhs,
        const pointer_type& rhs) noexcept
    {
        return lhs == rhs;
    }

    template<typename T>
    inline bool atomic_pointer_equal(
        const rpc::optimistic_ptr<T>& lhs,
        const rpc::optimistic_ptr<T>& rhs) noexcept
    {
        if (lhs.is_null() || rhs.is_null())
            return lhs.is_null() == rhs.is_null();
        return lhs.get_unsafe_only_for_testing() == rhs.get_unsafe_only_for_testing();
    }

    template<typename pointer_type> class atomic_smart_ptr
    {
    public:
        using value_type = pointer_type;

        static constexpr bool is_always_lock_free = false;

        atomic_smart_ptr() = default;

        atomic_smart_ptr(std::nullptr_t)
            : ptr_(nullptr)
        {
        }

        explicit atomic_smart_ptr(value_type desired)
            : ptr_(std::move(desired))
        {
        }

        atomic_smart_ptr(const atomic_smart_ptr&) = delete;
        atomic_smart_ptr& operator=(const atomic_smart_ptr&) = delete;

        void operator=(value_type desired) { store(std::move(desired)); }

        void operator=(std::nullptr_t) { store(nullptr); }

        bool is_lock_free() const noexcept { return false; }

        void store(
            value_type desired,
            std::memory_order order = std::memory_order_seq_cst)
        {
            (void)order;
            value_type old;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                old = std::move(ptr_);
                ptr_ = std::move(desired);
            }
        }

        value_type load(std::memory_order order = std::memory_order_seq_cst) const
        {
            (void)order;
            std::lock_guard<std::mutex> lock(mutex_);
            return ptr_;
        }

        operator value_type() const { return load(); }

        value_type exchange(
            value_type desired,
            std::memory_order order = std::memory_order_seq_cst)
        {
            (void)order;
            value_type old;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                old = std::move(ptr_);
                ptr_ = std::move(desired);
            }
            return old;
        }

        bool compare_exchange_weak(
            value_type& expected,
            value_type desired,
            std::memory_order success,
            std::memory_order failure)
        {
            return compare_exchange_impl(expected, std::move(desired), success, failure);
        }

        bool compare_exchange_weak(
            value_type& expected,
            value_type desired,
            std::memory_order order = std::memory_order_seq_cst)
        {
            return compare_exchange_impl(expected, std::move(desired), order, order);
        }

        bool compare_exchange_strong(
            value_type& expected,
            value_type desired,
            std::memory_order success,
            std::memory_order failure)
        {
            return compare_exchange_impl(expected, std::move(desired), success, failure);
        }

        bool compare_exchange_strong(
            value_type& expected,
            value_type desired,
            std::memory_order order = std::memory_order_seq_cst)
        {
            return compare_exchange_impl(expected, std::move(desired), order, order);
        }

    private:
        bool compare_exchange_impl(
            value_type& expected,
            value_type desired,
            std::memory_order success,
            std::memory_order failure)
        {
            // Mutex-backed atomic smart pointers accept memory_order parameters
            // for std::atomic API compatibility. The current implementation
            // provides one conservative synchronization model; these orders
            // become meaningful if this backend is replaced by a lock-free or
            // weaker-order implementation.
            (void)success;
            (void)failure;

            value_type observed;
            value_type old;
            bool exchanged = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (atomic_pointer_equal(ptr_, expected))
                {
                    old = std::move(ptr_);
                    ptr_ = std::move(desired);
                    exchanged = true;
                }
                else
                {
                    observed = ptr_;
                }
            }

            if (!exchanged)
                expected = std::move(observed);
            return exchanged;
        }

        mutable std::mutex mutex_;
        value_type ptr_;
    };
}

namespace std
{
#if !defined(__cpp_lib_atomic_shared_ptr)
    template<typename T> class atomic<shared_ptr<T>> : public rpc::polyfill::atomic_smart_ptr<shared_ptr<T>>
    {
        using base = rpc::polyfill::atomic_smart_ptr<shared_ptr<T>>;

    public:
        using base::base;
        using base::operator=;
    };
#endif

    template<typename T> class atomic<rpc::shared_ptr<T>> : public rpc::polyfill::atomic_smart_ptr<rpc::shared_ptr<T>>
    {
        using base = rpc::polyfill::atomic_smart_ptr<rpc::shared_ptr<T>>;

    public:
        using base::base;
        using base::operator=;
    };

    template<typename T>
    class atomic<rpc::optimistic_ptr<T>> : public rpc::polyfill::atomic_smart_ptr<rpc::optimistic_ptr<T>>
    {
        using base = rpc::polyfill::atomic_smart_ptr<rpc::optimistic_ptr<T>>;

    public:
        using base::base;
        using base::operator=;
    };
}
