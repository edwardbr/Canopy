/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#ifndef _GLIBCXX_UNIQUE_LOCK_H
#  define _GLIBCXX_UNIQUE_LOCK_H 1

#  include <bits/std_mutex.h>

namespace std
{
    template<typename mutex_type> class unique_lock
    {
    public:
        using mutex_type_alias = mutex_type;

        unique_lock() noexcept = default;

        explicit unique_lock(mutex_type& mutex)
            : mutex_(&mutex)
            , owns_(true)
        {
            mutex_->lock();
        }

        unique_lock(
            mutex_type& mutex,
            defer_lock_t) noexcept
            : mutex_(&mutex)
        {
        }

        unique_lock(
            mutex_type& mutex,
            try_to_lock_t)
            : mutex_(&mutex)
            , owns_(mutex.try_lock())
        {
        }

        unique_lock(
            mutex_type& mutex,
            adopt_lock_t) noexcept
            : mutex_(&mutex)
            , owns_(true)
        {
        }

        unique_lock(const unique_lock&) = delete;
        auto operator=(const unique_lock&) -> unique_lock& = delete;

        unique_lock(unique_lock&& other) noexcept
            : mutex_(other.mutex_)
            , owns_(other.owns_)
        {
            other.mutex_ = nullptr;
            other.owns_ = false;
        }

        auto operator=(unique_lock&& other) noexcept -> unique_lock&
        {
            if (this == &other)
                return *this;

            unlock_if_owned();
            mutex_ = other.mutex_;
            owns_ = other.owns_;
            other.mutex_ = nullptr;
            other.owns_ = false;
            return *this;
        }

        ~unique_lock() { unlock_if_owned(); }

        void lock()
        {
            if (mutex_ && !owns_)
            {
                mutex_->lock();
                owns_ = true;
            }
        }

        bool try_lock()
        {
            if (!mutex_ || owns_)
                return false;
            owns_ = mutex_->try_lock();
            return owns_;
        }

        void unlock() { unlock_if_owned(); }

        [[nodiscard]] bool owns_lock() const noexcept { return owns_; }
        explicit operator bool() const noexcept { return owns_lock(); }
        [[nodiscard]] auto mutex() const noexcept -> mutex_type* { return mutex_; }

        void swap(unique_lock& other) noexcept
        {
            auto* old_mutex = mutex_;
            mutex_ = other.mutex_;
            other.mutex_ = old_mutex;

            auto old_owns = owns_;
            owns_ = other.owns_;
            other.owns_ = old_owns;
        }

    private:
        void unlock_if_owned()
        {
            if (mutex_ && owns_)
            {
                mutex_->unlock();
                owns_ = false;
            }
        }

        mutex_type* mutex_ = nullptr;
        bool owns_ = false;
    };

    template<typename mutex_type>
    void swap(
        unique_lock<mutex_type>& lhs,
        unique_lock<mutex_type>& rhs) noexcept
    {
        lhs.swap(rhs);
    }
}

#endif
