/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/internal/executor/blocking_executor.h>
#include <rpc/internal/logger.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace rpc
{
    namespace
    {
        std::size_t resolve_thread_count(std::size_t requested) noexcept
        {
            if (requested != 0)
                return requested;
            auto hw = static_cast<std::size_t>(std::thread::hardware_concurrency());
            // Streaming transports take 2 dedicated threads (recv + send) per
            // connection. Reserve a few extra for dispatch and ad-hoc post()
            // callers so a single transport does not starve the service.
            return std::max<std::size_t>(hw, 4);
        }

        std::size_t resolve_max_thread_count(
            std::size_t initial_thread_count,
            std::size_t requested_max_thread_count) noexcept
        {
            if (requested_max_thread_count != 0)
                return std::max(initial_thread_count, requested_max_thread_count);

            auto doubled = initial_thread_count > (static_cast<std::size_t>(-1) / 2) ? initial_thread_count
                                                                                     : initial_thread_count * 2;
            return std::max<std::size_t>(std::max(initial_thread_count + 4, doubled), 8);
        }
    }

    blocking_executor::blocking_executor()
        : blocking_executor(options{})
    {
    }

    blocking_executor::blocking_executor(options opts)
        : max_thread_count_(resolve_max_thread_count(
              resolve_thread_count(opts.thread_count),
              opts.max_thread_count))
        , enable_adaptive_threads_(opts.enable_adaptive_threads)
    {
        auto count = resolve_thread_count(opts.thread_count);
        if (max_thread_count_ < count)
            max_thread_count_ = count;

        slots_.reserve(max_thread_count_);
        for (std::size_t i = 0; i < max_thread_count_; ++i)
            slots_.emplace_back(std::make_unique<worker_slot>());
        workers_.reserve(max_thread_count_);
        for (std::size_t i = 0; i < count; ++i)
            start_worker_locked(i);
    }

    blocking_executor::~blocking_executor()
    {
        shutdown();
    }

    bool blocking_executor::post(std::function<void()> fn)
    {
        if (!fn)
            return false;
        if (shutdown_.load(std::memory_order_acquire))
            return false;

        // Round-robin: a task posted from worker A lands on a different
        // worker's queue (usually). Work-stealing rebalances under load.
        auto active_workers = worker_count_.load(std::memory_order_acquire);
        if (active_workers == 0)
            return false;

        auto idx = dispatch_index_.fetch_add(1, std::memory_order_relaxed) % active_workers;
        {
            std::lock_guard<std::mutex> lock(slots_[idx]->mtx);
            // Re-check shutdown_ INSIDE the lock so the rest of the body is
            // serialised against shutdown's queue inspection. Without this
            // re-check, post() could push after shutdown observed all queues
            // empty, leaving an orphan task that workers never run.
            if (shutdown_.load(std::memory_order_relaxed))
                return false;
            slots_[idx]->queue.push_back(queued_task{std::move(fn)});
            // pending_tasks_ is mutated under the queue lock so workers'
            // (queue, counter) view is always consistent. A worker that sees
            // pending_tasks_ > 0 either finds the task in a queue or another
            // worker took it (and decremented the counter under that lock).
            pending_tasks_.fetch_add(1, std::memory_order_release);
        }

        // Wake exactly one idle worker. Holding wake_mtx_ momentarily ensures
        // the wait-condition re-check ordering: if a worker just observed
        // pending_tasks_ == 0 and is about to wait(), it will either see our
        // increment after we drop the lock, or have already entered wait()
        // and be reachable by notify_one.
        {
            std::lock_guard<std::mutex> lock(wake_mtx_);
        }
        wake_cv_.notify_one();
        maybe_grow();
        return true;
    }

    bool blocking_executor::schedule_after(std::chrono::milliseconds delay)
    {
        if (shutdown_.load(std::memory_order_acquire))
            return false;
        std::unique_lock<std::mutex> lock(timer_mtx_);
        return !timer_cv_.wait_for(lock, delay, [this] { return shutdown_.load(std::memory_order_acquire); });
    }

    void blocking_executor::shutdown() noexcept
    {
        bool expected = false;
        if (!shutdown_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return;

        // DRAIN semantics: do NOT clear pending queues. Workers continue
        // running until every queue is empty AND shutdown_ is observed, at
        // which point worker_loop() returns and the threads are joined.
        // This matches the documented contract that all callables posted
        // before shutdown() are run to completion. A subsequent post() (from
        // a peer or the calling thread) is rejected because shutdown_ is
        // already set — see post()'s inside-the-lock recheck.

        // Wake every sleeping worker so they observe shutdown_ and either
        // drain remaining work or exit.
        {
            std::lock_guard<std::mutex> lock(wake_mtx_);
        }
        wake_cv_.notify_all();
        timer_cv_.notify_all();

        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(worker_control_mtx_);
            workers.swap(workers_);
        }

        for (auto& t : workers)
        {
            if (t.joinable())
                t.join();
        }
        worker_count_.store(0, std::memory_order_release);
    }

    void blocking_executor::start_worker_locked(std::size_t index)
    {
        workers_.emplace_back([this, index] { worker_loop(index); });
        worker_count_.store(workers_.size(), std::memory_order_release);
    }

    void blocking_executor::maybe_grow()
    {
        if (!enable_adaptive_threads_)
            return;
        if (shutdown_.load(std::memory_order_acquire))
            return;
        if (pending_tasks_.load(std::memory_order_acquire) == 0)
            return;

        auto active_workers = worker_count_.load(std::memory_order_acquire);
        if (active_workers >= max_thread_count_)
            return;
        if (running_tasks_.load(std::memory_order_acquire) < active_workers)
            return;

        std::lock_guard<std::mutex> lock(worker_control_mtx_);
        if (shutdown_.load(std::memory_order_acquire))
            return;
        active_workers = workers_.size();
        if (active_workers >= max_thread_count_)
            return;
        if (running_tasks_.load(std::memory_order_acquire) < active_workers)
            return;

        start_worker_locked(active_workers);
        wake_cv_.notify_one();
    }

    bool blocking_executor::try_pop_own(
        std::size_t self_index,
        queued_task& out)
    {
        std::lock_guard<std::mutex> lock(slots_[self_index]->mtx);
        if (slots_[self_index]->queue.empty())
            return false;
        out = std::move(slots_[self_index]->queue.front());
        slots_[self_index]->queue.pop_front();
        // Decrement counter under the same lock as the queue pop so that
        // (pending_tasks_, queue sizes) stays consistent for the shutdown
        // exit condition. Without this, a worker could pop ahead of the
        // post-side increment and underflow the counter.
        pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    bool blocking_executor::try_steal(
        std::size_t self_index,
        queued_task& out)
    {
        // Scan peers, stealing one task from the first non-empty queue we
        // find. Steal from the BACK of the victim's deque (LIFO from
        // victim's perspective) to minimise contention with the victim's
        // own front-pops.
        auto n = worker_count_.load(std::memory_order_acquire);
        for (std::size_t offset = 1; offset < n; ++offset)
        {
            auto victim = (self_index + offset) % n;
            std::lock_guard<std::mutex> lock(slots_[victim]->mtx);
            if (slots_[victim]->queue.empty())
                continue;
            out = std::move(slots_[victim]->queue.back());
            slots_[victim]->queue.pop_back();
            pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
        return false;
    }

    void blocking_executor::worker_loop(std::size_t index)
    {
        for (;;)
        {
            queued_task task;

            // Take from own queue first (recently-dispatched-to-me tasks);
            // then steal from peers — this is what unsticks tasks
            // round-robined to a worker currently blocked in a long-running
            // streaming loop.
            if (try_pop_own(index, task) || try_steal(index, task))
            {
                try
                {
                    running_tasks_.fetch_add(1, std::memory_order_acq_rel);
                    maybe_grow();
                    task.fn();
                }
                catch (const std::exception& ex)
                {
                    // Worker threads must not propagate exceptions; posted
                    // callables own their own error handling.
                    RPC_CRITICAL("blocking_executor worker task threw: {}", ex.what());
                }
                catch (...)
                {
                    // Worker threads must not propagate exceptions; posted
                    // callables own their own error handling.
                    RPC_CRITICAL("blocking_executor worker task threw an unknown exception");
                }
                running_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                maybe_grow();
                continue;
            }

            // No work anywhere. Wait until post() or shutdown() signals.
            // Drain contract: only exit when shutdown_ is set AND every
            // queue is observed empty (pending_tasks_ == 0).
            std::unique_lock<std::mutex> lock(wake_mtx_);
            wake_cv_.wait(
                lock,
                [this]
                {
                    return shutdown_.load(std::memory_order_acquire) || pending_tasks_.load(std::memory_order_acquire) > 0;
                });
            if (shutdown_.load(std::memory_order_acquire) && pending_tasks_.load(std::memory_order_acquire) == 0)
                return;
        }
    }
} // namespace rpc
