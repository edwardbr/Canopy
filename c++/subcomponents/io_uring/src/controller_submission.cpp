/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        static constexpr uint32_t operation_wait_attempt_limit = 4'000'000;

        // Gives a CPU-only fallback wait hint when no scheduler is available to
        // suspend the current coroutine between SQ/CQ polling attempts.
        void relax_spin() noexcept
        {
#if defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
            std::atomic_signal_fence(std::memory_order_acq_rel);
        }
    } // namespace

    // Shared operation wrapper for non-NOP SQEs. It validates the cached ring
    // descriptor, allocates an operation record, submits the caller-provided SQE,
    // wakes SQPOLL if required, and waits for the matching CQE.
    CORO_TASK(operation_result)
    controller::submit_operation(
        fill_sqe_callback fill_sqe,
        void* context)
    {
        if (!fill_sqe)
        {
            CO_RETURN operation_result{rpc::error::INVALID_DATA(), 0, 0};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }

        const auto ring_data = cached_iouring_data_copy();
        if (!detail::validate_ring_data_for_direct_ring(ring_data))
        {
            RPC_WARNING(
                "direct io_uring operation invalid ring data descriptor_version={} setup_flags={} sq_entries={} "
                "cq_entries={} sq_array_ptr={}",
                ring_data.descriptor_version,
                ring_data.setup.setup_flags,
                ring_data.submission_queue.sq_ring_entries,
                ring_data.completion_queue.cq_ring_entries,
                ring_data.submission_queue.sq_array_ptr);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        if (!detail::has_sqpoll(ring_data))
        {
            RPC_WARNING("direct io_uring operation requires SQPOLL setup_flags={}", ring_data.setup.setup_flags);
            CO_RETURN operation_result{rpc::error::INCOMPATIBLE_SERVICE(), 0, 0};
        }

        std::shared_ptr<detail::direct_ring_operation> operation;
        try
        {
            operation = std::make_shared<detail::direct_ring_operation>();
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring operation");
            std::terminate();
        }

        err = CO_AWAIT submit_prepared_operation(ring_data, operation, fill_sqe, context);
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, operation->cqe_result, operation->cqe_flags};
        }

        if (detail::sqpoll_needs_wakeup(ring_data))
        {
            err = CO_AWAIT notify_submitted(ring_data, 1);
            if (err != rpc::error::OK())
            {
                resume_completed_operations(operation_engine_.fail_all_operations(err));
                RPC_WARNING("direct io_uring operation wake_iouring failed error={}", err);
                CO_RETURN operation_result{err, operation->cqe_result, operation->cqe_flags};
            }
        }

        err = CO_AWAIT wait_for_operation(ring_data, operation);
        CO_RETURN operation_result{err, operation->cqe_result, operation->cqe_flags};
    }

    // Submits two SQEs as one linked kernel operation. The first SQE is the
    // operation the caller waits for; the second SQE normally carries a linked
    // timeout and has its own operation table entry so its CQE is still matched
    // and trusted-drained later.
    CORO_TASK(operation_result)
    controller::submit_linked_operation(
        fill_linked_sqe_callback fill_sqes,
        void* context,
        std::shared_ptr<void> linked_keep_alive)
    {
        if (!fill_sqes)
        {
            CO_RETURN operation_result{rpc::error::INVALID_DATA(), 0, 0};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, 0, 0};
        }

        const auto ring_data = cached_iouring_data_copy();
        if (!detail::validate_ring_data_for_direct_ring(ring_data))
        {
            RPC_WARNING(
                "direct io_uring linked operation invalid ring data descriptor_version={} setup_flags={} "
                "sq_entries={} "
                "cq_entries={} sq_array_ptr={}",
                ring_data.descriptor_version,
                ring_data.setup.setup_flags,
                ring_data.submission_queue.sq_ring_entries,
                ring_data.completion_queue.cq_ring_entries,
                ring_data.submission_queue.sq_array_ptr);
            CO_RETURN operation_result{rpc::error::PROTOCOL_ERROR(), 0, 0};
        }

        if (!detail::has_sqpoll(ring_data))
        {
            RPC_WARNING("direct io_uring linked operation requires SQPOLL setup_flags={}", ring_data.setup.setup_flags);
            CO_RETURN operation_result{rpc::error::INCOMPATIBLE_SERVICE(), 0, 0};
        }

        std::shared_ptr<detail::direct_ring_operation> primary_operation;
        std::shared_ptr<detail::direct_ring_operation> linked_operation;
        try
        {
            primary_operation = std::make_shared<detail::direct_ring_operation>();
            linked_operation = std::make_shared<detail::direct_ring_operation>();
            linked_operation->keep_alive = std::move(linked_keep_alive);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating linked direct io_uring operations");
            std::terminate();
        }

        err = CO_AWAIT submit_prepared_linked_operation(ring_data, primary_operation, linked_operation, fill_sqes, context);
        if (err != rpc::error::OK())
        {
            CO_RETURN operation_result{err, primary_operation->cqe_result, primary_operation->cqe_flags};
        }

        if (detail::sqpoll_needs_wakeup(ring_data))
        {
            err = CO_AWAIT notify_submitted(ring_data, 2);
            if (err != rpc::error::OK())
            {
                resume_completed_operations(operation_engine_.fail_all_operations(err));
                RPC_WARNING("direct io_uring linked operation wake_iouring failed error={}", err);
                CO_RETURN operation_result{err, primary_operation->cqe_result, primary_operation->cqe_flags};
            }
        }

        err = CO_AWAIT wait_for_operation(ring_data, primary_operation);

        // The linked timeout CQE may arrive just after the primary operation.
        // Drain once here so cooperative users do not leave a harmless
        // cancellation CQE pending until their next io_uring operation.
        auto pump_result = operation_engine_.pump_completions(ring_data);
        consume_pump_result(pump_result);
        if (err == rpc::error::OK() && pump_result.error_code != rpc::error::OK())
        {
            err = pump_result.error_code;
        }

        CO_RETURN operation_result{err, primary_operation->cqe_result, primary_operation->cqe_flags};
    }

    // Specialized NOP submit loop used by the measurement smoke tests. It keeps
    // trying while the SQ is temporarily full and lets completion pumping free
    // capacity before timing out.
    CORO_TASK(int)
    controller::submit_no_op(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& operation)
    {
        int waiter_error = rpc::error::OK();
        auto waiter = make_submission_waiter(1, waiter_error);
        if (!waiter)
        {
            CO_RETURN waiter_error;
        }

        for (uint32_t attempt = 0; attempt < operation_wait_attempt_limit; ++attempt)
        {
            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN shutdown_err;
            }

            if (!can_attempt_submission(waiter))
            {
                if (waiter->error_code != rpc::error::OK())
                {
                    CO_RETURN waiter->error_code;
                }

                measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
                CO_AWAIT wait_before_next_poll();
                continue;
            }

            measurements_.submit_attempts.fetch_add(1, std::memory_order_relaxed);
            auto submit_result = operation_engine_.try_submit_no_op(ring_data, operation);
            consume_submit_result(submit_result);
            if (submit_result.error_code != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN submit_result.error_code;
            }

            if (submit_result.submitted)
            {
                complete_submission_waiter(waiter);
                CO_RETURN rpc::error::OK();
            }

            measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
            CO_AWAIT wait_before_next_poll();
        }

        cancel_submission_waiter(waiter);
        RPC_WARNING("direct io_uring no_op timed out waiting for submission capacity");
        CO_RETURN rpc::error::RESOURCE_EXHAUSTED();
    }

    // General submit loop for an already-created operation record. The fill
    // callback writes the operation-specific fields into a zeroed SQE while the
    // operation engine owns the SQ lock.
    CORO_TASK(int)
    controller::submit_prepared_operation(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& operation,
        fill_sqe_callback fill_sqe,
        void* context)
    {
        int waiter_error = rpc::error::OK();
        auto waiter = make_submission_waiter(1, waiter_error);
        if (!waiter)
        {
            CO_RETURN waiter_error;
        }

        for (uint32_t attempt = 0; attempt < operation_wait_attempt_limit; ++attempt)
        {
            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN shutdown_err;
            }

            if (!can_attempt_submission(waiter))
            {
                if (waiter->error_code != rpc::error::OK())
                {
                    CO_RETURN waiter->error_code;
                }

                measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
                CO_AWAIT wait_before_next_poll();
                continue;
            }

            measurements_.submit_attempts.fetch_add(1, std::memory_order_relaxed);
            auto submit_result = operation_engine_.try_submit(
                ring_data, operation, [fill_sqe, context](detail::sqe_64& sqe) { fill_sqe(sqe, context); });
            consume_submit_result(submit_result);
            if (submit_result.error_code != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN submit_result.error_code;
            }

            if (submit_result.submitted)
            {
                complete_submission_waiter(waiter);
                CO_RETURN rpc::error::OK();
            }

            measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
            CO_AWAIT wait_before_next_poll();
        }

        cancel_submission_waiter(waiter);
        RPC_WARNING("direct io_uring operation timed out waiting for submission capacity");
        CO_RETURN rpc::error::RESOURCE_EXHAUSTED();
    }

    // Submit loop for a two-SQE linked operation. Backpressure is measured once
    // per attempt and retried cooperatively, exactly like the single-SQE path.
    CORO_TASK(int)
    controller::submit_prepared_linked_operation(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& primary_operation,
        const std::shared_ptr<detail::direct_ring_operation>& linked_operation,
        fill_linked_sqe_callback fill_sqes,
        void* context)
    {
        int waiter_error = rpc::error::OK();
        auto waiter = make_submission_waiter(2, waiter_error);
        if (!waiter)
        {
            CO_RETURN waiter_error;
        }

        for (uint32_t attempt = 0; attempt < operation_wait_attempt_limit; ++attempt)
        {
            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN shutdown_err;
            }

            if (!can_attempt_submission(waiter))
            {
                if (waiter->error_code != rpc::error::OK())
                {
                    CO_RETURN waiter->error_code;
                }

                measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
                CO_AWAIT wait_before_next_poll();
                continue;
            }

            measurements_.submit_attempts.fetch_add(1, std::memory_order_relaxed);
            auto submit_result = operation_engine_.try_submit_linked(
                ring_data,
                primary_operation,
                linked_operation,
                [fill_sqes, context](detail::sqe_64& primary_sqe, detail::sqe_64& linked_sqe)
                { fill_sqes(primary_sqe, linked_sqe, context); });
            consume_submit_result(submit_result);
            if (submit_result.error_code != rpc::error::OK())
            {
                cancel_submission_waiter(waiter);
                CO_RETURN submit_result.error_code;
            }

            if (submit_result.submitted)
            {
                complete_submission_waiter(waiter);
                CO_RETURN rpc::error::OK();
            }

            measurements_.submit_backpressure.fetch_add(1, std::memory_order_relaxed);
            CO_AWAIT wait_before_next_poll();
        }

        cancel_submission_waiter(waiter);
        RPC_WARNING("direct io_uring linked operation timed out waiting for submission capacity");
        CO_RETURN rpc::error::RESOURCE_EXHAUSTED();
    }

    std::shared_ptr<controller::submission_waiter> controller::make_submission_waiter(
        uint32_t required_sqe_count,
        int& error_code) const
    {
        if (required_sqe_count == 0)
        {
            error_code = rpc::error::INVALID_DATA();
            return {};
        }

        try
        {
            auto waiter = std::make_shared<submission_waiter>();
            waiter->required_sqe_count = required_sqe_count;
            error_code = rpc::error::OK();
            return waiter;
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring submission waiter");
            std::terminate();
        }

        return {};
    }

    bool controller::can_attempt_submission(const std::shared_ptr<submission_waiter>& waiter)
    {
        if (!waiter)
        {
            return false;
        }

        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            waiter->error_code = shutdown_err;
            return false;
        }

        if (waiter->error_code != rpc::error::OK())
        {
            return false;
        }

        std::lock_guard<rpc::spin_mutex> lock(submission_waiter_mutex_);
        shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            waiter->error_code = shutdown_err;
            return false;
        }

        if (!waiter->queued)
        {
            try
            {
                submission_waiters_.push_back(waiter);
                waiter->queued = true;
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while queuing direct io_uring submission waiter");
                std::terminate();
            }
        }

        return !submission_waiters_.empty() && submission_waiters_.front() == waiter;
    }

    void controller::complete_submission_waiter(const std::shared_ptr<submission_waiter>& waiter) noexcept
    {
        if (!waiter)
        {
            return;
        }

        std::lock_guard<rpc::spin_mutex> lock(submission_waiter_mutex_);
        if (waiter->queued && !submission_waiters_.empty() && submission_waiters_.front() == waiter)
        {
            submission_waiters_.pop_front();
        }
        else
        {
            auto found = std::find(submission_waiters_.begin(), submission_waiters_.end(), waiter);
            if (found != submission_waiters_.end())
            {
                submission_waiters_.erase(found);
            }
        }
        waiter->queued = false;
    }

    void controller::cancel_submission_waiter(const std::shared_ptr<submission_waiter>& waiter) noexcept
    {
        complete_submission_waiter(waiter);
    }

    void controller::fail_submission_waiters(int error_code) noexcept
    {
        std::lock_guard<rpc::spin_mutex> lock(submission_waiter_mutex_);
        for (auto& waiter : submission_waiters_)
        {
            if (waiter)
            {
                waiter->error_code = error_code;
                waiter->queued = false;
            }
        }
        submission_waiters_.clear();
    }

    // Chooses the wait implementation for one submitted operation. Proactor mode
    // uses a detached pump when a scheduler exists; otherwise the waiter pumps
    // cooperatively.
    CORO_TASK(int)
    controller::wait_for_operation(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& operation)
    {
        if (wait_strategy_ == wait_strategy::proactor && scheduler_)
        {
            CO_RETURN CO_AWAIT wait_for_operation_proactor(ring_data, operation);
        }

        CO_RETURN CO_AWAIT wait_for_operation_cooperative(ring_data, operation);
    }

    // Waits by repeatedly pumping the CQ from the current coroutine. Each pass
    // drains any completions, checks whether this operation completed, then
    // yields/spins before polling again.
    CORO_TASK(int)
    controller::wait_for_operation_cooperative(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& operation)
    {
        for (uint32_t attempt = 0; attempt < operation_wait_attempt_limit; ++attempt)
        {
            auto pump_result = operation_engine_.pump_completions(ring_data);
            consume_pump_result(pump_result);
            if (operation->completed.load(std::memory_order_acquire))
            {
                CO_RETURN operation->error_code;
            }
            if (pump_result.error_code != rpc::error::OK())
            {
                CO_RETURN pump_result.error_code;
            }

            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                CO_RETURN shutdown_err;
            }

            // This is still cooperative polling, but it now behaves like a
            // scheduler-friendly precursor to a proactor: every wait pumps
            // the shared CQ once, then yields so another operation or pump
            // can run. A future controller-owned pump can use the same
            // user_data operation table to resume exactly the waiter whose
            // CQE arrived.
            CO_AWAIT wait_before_next_poll();
        }

        RPC_WARNING(
            "direct io_uring operation timed out user_data={} submitted={}",
            operation->user_data,
            operation->submitted.load(std::memory_order_acquire));
        CO_RETURN rpc::error::CALL_TIMEOUT();
    }

    struct controller::operation_completion_awaiter
    {
        controller& controller;
        std::shared_ptr<detail::direct_ring_operation> operation;

        // Avoids suspension if the operation completed before this awaiter was
        // reached.
        bool await_ready() const noexcept { return !operation || operation->completed.load(std::memory_order_acquire); }

        // Stores the awaiting coroutine handle in the operation table so the
        // proactor pump can resume exactly this waiter when its CQE arrives.
        bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
        {
            controller.measurements_.proactor_waiter_suspends.fetch_add(1, std::memory_order_relaxed);
            return controller.operation_engine_.register_waiter(operation, awaiting_coroutine);
        }

        // Converts the completed operation record back into an RPC error code.
        int await_resume() const noexcept
        {
            return operation ? operation->error_code : rpc::error::OPERATION_CANCELLED();
        }
    };

    // Waits by registering the current coroutine as the operation waiter and
    // ensuring a scheduler-owned completion pump is running to resume it.
    CORO_TASK(int)
    controller::wait_for_operation_proactor(
        const data& ring_data,
        const std::shared_ptr<detail::direct_ring_operation>& operation)
    {
        if (operation->completed.load(std::memory_order_acquire))
        {
            CO_RETURN operation->error_code;
        }

        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            CO_RETURN shutdown_err;
        }

        if (!request_completion_pump(ring_data))
        {
            CO_RETURN CO_AWAIT wait_for_operation_cooperative(ring_data, operation);
        }

        CO_RETURN CO_AWAIT operation_completion_awaiter{*this, operation};
    }

    // Starts a detached completion pump if one is not already active. Multiple
    // waiters share the same pump so a burst of operations does not spawn one
    // polling coroutine per operation.
    bool controller::request_completion_pump(const data& ring_data) noexcept
    {
        if (!scheduler_)
        {
            return false;
        }

        if (is_shutdown_requested())
        {
            return false;
        }

        bool expected = false;
        if (!completion_pump_active_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }

        measurements_.proactor_pump_starts.fetch_add(1, std::memory_order_relaxed);
        auto task = completion_pump_loop(ring_data);
        if (scheduler_->spawn_detached(std::move(task)))
        {
            return true;
        }

        completion_pump_active_.store(false, std::memory_order_release);
        measurements_.proactor_start_failures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Scheduler task that drains the shared CQ until there are no pending
    // operations or an error occurs, resuming waiters as their CQEs are matched.
    CORO_TASK(void) controller::completion_pump_loop(data ring_data)
    {
        for (uint32_t attempt = 0; attempt < operation_wait_attempt_limit; ++attempt)
        {
            if (is_shutdown_requested())
            {
                break;
            }

            measurements_.proactor_pump_iterations.fetch_add(1, std::memory_order_relaxed);

            auto pump_result = operation_engine_.pump_completions(ring_data);
            consume_pump_result(pump_result);
            if (pump_result.error_code != rpc::error::OK())
            {
                break;
            }

            if (!operation_engine_.has_pending_operations())
            {
                break;
            }

            CO_AWAIT wait_before_next_poll();
        }

        completion_pump_active_.store(false, std::memory_order_release);
        if (operation_engine_.has_pending_operations())
        {
            request_completion_pump(ring_data);
        }

        CO_RETURN;
    }

    // Gives other coroutines a chance to run between repeated submission or CQ
    // polling attempts. Without a scheduler it degrades to a short CPU pause.
    CORO_TASK(void) controller::wait_before_next_poll()
    {
        if (scheduler_)
        {
            measurements_.scheduler_yields.fetch_add(1, std::memory_order_relaxed);
            CO_AWAIT scheduler_->schedule();
        }
        else
        {
            measurements_.local_relax_spins.fetch_add(1, std::memory_order_relaxed);
            relax_spin();
        }
        CO_RETURN;
    }
} // namespace rpc::io_uring
