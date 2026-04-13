/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/scheduler.hpp>
#else
#  include <condition_variable>
#  include <mutex>
#endif

namespace rpc
{
#ifdef CANOPY_BUILD_COROUTINE
    class event
    {
    public:
        void set() { event_.set(); }
        void reset() { event_.reset(); }
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

        void set()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                signaled_ = true;
            }
            cv_.notify_all();
        }

        void reset()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signaled_ = false;
        }

        void wait() const
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return signaled_; });
        }

    private:
        mutable std::mutex mutex_;
        mutable std::condition_variable cv_;
        bool signaled_;
    };
#endif
}
