/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <atomic>

namespace rpc
{
    // A tiny non-blocking mutex for very short critical sections.
    //
    // This is useful in places such as enclave-local bookkeeping where blocking
    // primitives may not be available. It is not an async mutex and must never
    // be held across CO_AWAIT or any other operation that can suspend.
    class spin_mutex
    {
    public:
        void lock() noexcept
        {
            while (locked_.test_and_set(std::memory_order_acquire))
            {
                std::atomic_signal_fence(std::memory_order_acq_rel);
            }
        }

        void unlock() noexcept { locked_.clear(std::memory_order_release); }

    private:
        std::atomic_flag locked_ = ATOMIC_FLAG_INIT;
    };
} // namespace rpc
