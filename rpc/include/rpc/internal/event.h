/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE
#include <coro/io_scheduler.hpp>
#else
#include <mutex>
#include <condition_variable>
#endif

namespace rpc
{
#ifdef CANOPY_BUILD_COROUTINE
    class event
    {
    public:
        // Signal the event: Wake all waiting threads
        void set() { event_.set(); }

        // Reset the event: Future calls to wait() will block
        void reset() { event_.reset(); }

        // Block until the event is set
        CORO_TASK(void) wait() const { CO_AWAIT event_; }

    private:
        coro::event event_;
    };
#else
    class event
    {
    public:
        explicit event()
            : signaled_(true)
        {
        }

        // Signal the event: Wake all waiting threads
        void set()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                signaled_ = true;
            }
            cv_.notify_all(); // Wake everyone
        }

        // Reset the event: Future calls to wait() will block
        void reset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signaled_ = false;
        }

        // Block until the event is set
        void wait() const
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // The lambda handles "spurious wakeups"
            cv_.wait(lock, [this] { return signaled_; });
        }

    private:
        mutable std::mutex mutex_;
        mutable std::condition_variable cv_;
        bool signaled_;
    };
#endif
}
