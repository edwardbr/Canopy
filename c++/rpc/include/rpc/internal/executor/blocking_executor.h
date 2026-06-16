/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

// rpc::blocking_executor — std::thread-pool back end for rpc::executor in
// non-coroutine builds. Holds N worker threads, each with its OWN FIFO queue.
// post() round-robins submissions across the per-worker queues; idle workers
// steal from peers whose queues are non-empty.
//
// Why per-worker + stealing rather than a single shared queue: the streaming
// transport posts long-running loops (receive_consumer_loop, send_producer_loop)
// that occupy a worker for the lifetime of a connection. While running, that
// worker cannot drain its own queue. A shared-queue design would let other
// idle workers grab anything posted from inside the loop — which is fine in
// the common case — but pins observability: there is no notion of "the task
// I dispatched goes to a different thread." Per-worker queues make that
// dispatch explicit (round-robin push) and work-stealing prevents a
// long-running worker from starving its queue (peers steal from the back).
//
// API:
//   post(fn)              dispatch a callable to a worker. Round-robin
//                         across queues. Returns false after shutdown.
//   schedule()            NO-OP. Callers that need write-readiness must use
//                         poll(POLLOUT) (or equivalent) on the underlying fd.
//   schedule_after(d)     timed wait that participates in shutdown — wakes
//                         early if shutdown() is invoked.
//   shutdown()            DRAIN: flip the shutdown flag, signal workers,
//                         and wait for every callable already in a queue
//                         to run before joining. Subsequent post() calls
//                         return false. Idempotent.
//
// Optionality (see project_executor_is_optional_in_blocking.md): the executor
// is opt-in for blocking builds. Existing users that don't need streaming
// continue running synchronously without one. Streaming requires it.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rpc
{
    class blocking_executor
    {
    public:
        struct options
        {
            // Workers pinned to streaming loops (receive, send) consume one
            // thread each for the lifetime of a connection. Default leaves
            // room for moderate fan-out without overcommitting.
            std::size_t thread_count{0};

            // Upper bound for adaptive worker growth. Zero derives a bounded
            // host default from thread_count/hardware_concurrency.
            std::size_t max_thread_count{0};

            // Host blocking builds can pin every worker in long-running waits.
            // When enabled, queued work plus all active workers running grows
            // the pool up to max_thread_count.
            bool enable_adaptive_threads{true};
        };

        blocking_executor();
        explicit blocking_executor(options opts);
        blocking_executor(const blocking_executor&) = delete;
        blocking_executor(blocking_executor&&) = delete;
        blocking_executor& operator=(const blocking_executor&) = delete;
        blocking_executor& operator=(blocking_executor&&) = delete;
        ~blocking_executor();

        bool post(std::function<void()> fn);

        // Cooperative yield. NO-OP in blocking mode.
        void schedule() { }

        // Sleep up to delay, returning early if shutdown is requested.
        // Returns true if the delay elapsed normally, false on shutdown.
        bool schedule_after(std::chrono::milliseconds delay);

        void shutdown() noexcept;

        [[nodiscard]] bool is_shutdown() const noexcept { return shutdown_.load(std::memory_order_acquire); }

        [[nodiscard]] std::size_t worker_count() const noexcept
        {
            return worker_count_.load(std::memory_order_acquire);
        }

    private:
        struct queued_task
        {
            std::function<void()> fn;
        };

        struct worker_slot
        {
            std::deque<queued_task> queue;
            mutable std::mutex mtx;
        };

        void start_worker_locked(std::size_t index);
        void maybe_grow();
        void worker_loop(std::size_t index);
        bool try_pop_own(
            std::size_t self_index,
            queued_task& out);
        bool try_steal(
            std::size_t self_index,
            queued_task& out);

        std::vector<std::unique_ptr<worker_slot>> slots_;
        std::vector<std::thread> workers_;
        mutable std::mutex worker_control_mtx_;
        std::size_t max_thread_count_{0};
        bool enable_adaptive_threads_{true};
        std::atomic<std::size_t> worker_count_{0};
        std::atomic<std::size_t> running_tasks_{0};

        // Global condition for waking idle workers. notify_one on post(),
        // notify_all on shutdown(). The cv is decoupled from any single
        // queue so any worker can wake regardless of which queue grew.
        mutable std::mutex wake_mtx_;
        std::condition_variable wake_cv_;
        std::atomic<std::size_t> pending_tasks_{0};

        std::atomic<std::size_t> dispatch_index_{0};
        std::atomic<bool> shutdown_{false};

        mutable std::mutex timer_mtx_;
        std::condition_variable timer_cv_;
    };
} // namespace rpc
