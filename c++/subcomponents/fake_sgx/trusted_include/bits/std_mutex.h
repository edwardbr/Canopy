/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#ifndef _GLIBCXX_MUTEX_H
#  define _GLIBCXX_MUTEX_H 1

#  include <pthread.h>
#  include <time.h>

namespace std
{
    struct defer_lock_t
    {
        explicit defer_lock_t() = default;
    };

    struct try_to_lock_t
    {
        explicit try_to_lock_t() = default;
    };

    struct adopt_lock_t
    {
        explicit adopt_lock_t() = default;
    };

    inline constexpr defer_lock_t defer_lock{};
    inline constexpr try_to_lock_t try_to_lock{};
    inline constexpr adopt_lock_t adopt_lock{};

    class mutex
    {
    public:
        using native_handle_type = pthread_mutex_t*;

        mutex() noexcept { pthread_mutex_init(&mutex_, nullptr); }
        mutex(const mutex&) = delete;
        auto operator=(const mutex&) -> mutex& = delete;
        ~mutex() { pthread_mutex_destroy(&mutex_); }

        void lock() { pthread_mutex_lock(&mutex_); }
        bool try_lock() noexcept { return pthread_mutex_trylock(&mutex_) == 0; }
        void unlock() { pthread_mutex_unlock(&mutex_); }
        native_handle_type native_handle() noexcept { return &mutex_; }

    private:
        pthread_mutex_t mutex_{};
    };

    class __condvar
    {
    public:
        __condvar() noexcept { pthread_cond_init(&cond_, nullptr); }
        __condvar(const __condvar&) = delete;
        auto operator=(const __condvar&) -> __condvar& = delete;
        ~__condvar() { pthread_cond_destroy(&cond_); }

        auto native_handle() noexcept -> pthread_cond_t* { return &cond_; }
        void wait(mutex& mutex) { pthread_cond_wait(&cond_, mutex.native_handle()); }
        void wait_until(
            mutex& mutex,
            timespec& abs_time)
        {
            pthread_cond_timedwait(&cond_, mutex.native_handle(), &abs_time);
        }
        void notify_one() noexcept { pthread_cond_signal(&cond_); }
        void notify_all() noexcept { pthread_cond_broadcast(&cond_); }

    private:
        pthread_cond_t cond_{};
    };

    template<typename mutex_type> class lock_guard
    {
    public:
        using mutex_type_alias = mutex_type;

        explicit lock_guard(mutex_type& mutex)
            : mutex_(mutex)
        {
            mutex_.lock();
        }

        lock_guard(
            mutex_type& mutex,
            adopt_lock_t)
            : mutex_(mutex)
        {
        }

        lock_guard(const lock_guard&) = delete;
        auto operator=(const lock_guard&) -> lock_guard& = delete;

        ~lock_guard() { mutex_.unlock(); }

    private:
        mutex_type& mutex_;
    };
}

#endif
