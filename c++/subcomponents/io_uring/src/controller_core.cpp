/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <mutex>
#include <utility>

namespace rpc::io_uring
{
    // Binds the enclave-side controller to the host RPC interface and, when
    // available, the scheduler used for cooperative/proactor waits.
    controller::controller(rpc::coro::scheduler* scheduler)
        : scheduler_(scheduler)
    {
    }

    // Clears all per-controller counters so a test or caller can measure the
    // next operation burst without carrying over previous samples.
    void controller::atomic_measurements::reset() noexcept
    {
        no_op_calls.store(0, std::memory_order_relaxed);
        no_op_successes.store(0, std::memory_order_relaxed);
        no_op_failures.store(0, std::memory_order_relaxed);
        submit_attempts.store(0, std::memory_order_relaxed);
        submit_backpressure.store(0, std::memory_order_relaxed);
        completion_pump_calls.store(0, std::memory_order_relaxed);
        completion_entries.store(0, std::memory_order_relaxed);
        scheduler_yields.store(0, std::memory_order_relaxed);
        local_relax_spins.store(0, std::memory_order_relaxed);
        host_wake_calls.store(0, std::memory_order_relaxed);
        proactor_pump_starts.store(0, std::memory_order_relaxed);
        proactor_pump_iterations.store(0, std::memory_order_relaxed);
        proactor_waiter_suspends.store(0, std::memory_order_relaxed);
        proactor_resumes.store(0, std::memory_order_relaxed);
        proactor_start_failures.store(0, std::memory_order_relaxed);
        total_no_op_ticks.store(0, std::memory_order_relaxed);
        max_no_op_ticks.store(0, std::memory_order_relaxed);
    }

    // Takes a non-blocking copy of the atomic measurement counters for logging
    // or test assertions.
    controller_measurements controller::atomic_measurements::snapshot() const noexcept
    {
        return {no_op_calls.load(std::memory_order_relaxed),
            no_op_successes.load(std::memory_order_relaxed),
            no_op_failures.load(std::memory_order_relaxed),
            submit_attempts.load(std::memory_order_relaxed),
            submit_backpressure.load(std::memory_order_relaxed),
            completion_pump_calls.load(std::memory_order_relaxed),
            completion_entries.load(std::memory_order_relaxed),
            scheduler_yields.load(std::memory_order_relaxed),
            local_relax_spins.load(std::memory_order_relaxed),
            host_wake_calls.load(std::memory_order_relaxed),
            proactor_pump_starts.load(std::memory_order_relaxed),
            proactor_pump_iterations.load(std::memory_order_relaxed),
            proactor_waiter_suspends.load(std::memory_order_relaxed),
            proactor_resumes.load(std::memory_order_relaxed),
            proactor_start_failures.load(std::memory_order_relaxed),
            total_no_op_ticks.load(std::memory_order_relaxed),
            max_no_op_ticks.load(std::memory_order_relaxed)};
    }

    // Selects how completion waits are driven: cooperative polling by the
    // waiter itself, or a scheduler-owned proactor pump.
    void controller::set_wait_strategy(wait_strategy strategy) noexcept
    {
        wait_strategy_ = strategy;
    }

    // Returns the current completion wait strategy so tests can confirm which
    // path they are exercising.
    wait_strategy controller::get_wait_strategy() const noexcept
    {
        return wait_strategy_;
    }

    // Resets counters through the measurement wrapper; callers must ensure no
    // old operations are still updating the same counters.
    void controller::reset_measurements() noexcept
    {
        measurements_.reset();
    }

    // Returns a stable value snapshot of counters without exposing the atomic
    // storage used internally.
    controller_measurements controller::measurements() const noexcept
    {
        return measurements_.snapshot();
    }

    // Returns true after shutdown has started. Once this flips, operation
    // admission paths must not touch the untrusted rings for new work.
    bool controller::is_shutdown_requested() const noexcept
    {
        return lifecycle_state_.load(std::memory_order_acquire) != lifecycle_state::running;
    }

    bool controller::can_accept_work() const noexcept
    {
        return !is_shutdown_requested();
    }

    int controller::shutdown_error() const noexcept
    {
        if (!is_shutdown_requested())
        {
            return rpc::error::OK();
        }

        const auto error_code = shutdown_error_code_.load(std::memory_order_acquire);
        return error_code == rpc::error::OK() ? rpc::error::RESOURCE_CLOSED() : error_code;
    }

    // Starts controller shutdown from a non-awaiting context. It fails all
    // enclave-owned operation records and queued admission waiters; the host
    // still owns the kernel ring and will release it with the host controller.
    void controller::request_shutdown() noexcept
    {
        request_shutdown(rpc::error::OPERATION_CANCELLED());
    }

    void controller::request_shutdown(int error_code) noexcept
    {
        if (error_code == rpc::error::OK())
        {
            error_code = rpc::error::OPERATION_CANCELLED();
        }

        auto expected = lifecycle_state::running;
        if (!lifecycle_state_.compare_exchange_strong(
                expected, lifecycle_state::stopping, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return;
        }

        shutdown_error_code_.store(error_code, std::memory_order_release);
        fail_submission_waiters(error_code);
        fail_host_buffer_waiters(error_code);
        resume_completed_operations(operation_engine_.fail_all_operations(error_code));
        {
            std::lock_guard<rpc::spin_mutex> lock(cache_mutex_);
            cached_iouring_data_ = {};
            has_cached_iouring_data_ = false;
        }
        completion_pump_active_.store(false, std::memory_order_release);
        lifecycle_state_.store(lifecycle_state::stopped, std::memory_order_release);
    }

    CORO_TASK(int) controller::shutdown()
    {
        request_shutdown();
        CO_RETURN rpc::error::OK();
    }

    // Performs the narrow host RPC needed to wake an SQPOLL kernel thread when
    // the ring sets IORING_SQ_NEED_WAKEUP.
    CORO_TASK(int) controller::wake_host_iouring()
    {
        auto err = shutdown_error();
        if (err != rpc::error::OK())
        {
            CO_RETURN err;
        }

        measurements_.host_wake_calls.fetch_add(1, std::memory_order_relaxed);
        CO_RETURN CO_AWAIT inner_wake_host_iouring();
    }

    // Returns the cached io_uring descriptor to callers, fetching it from the
    // host first if this controller has not cached it yet.
    CORO_TASK(int) controller::get_iouring_data(data& ring_data)
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN err;
        }

        ring_data = cached_iouring_data_copy();
        CO_RETURN rpc::error::OK();
    }

    // Refreshes the descriptor snapshot from the host RPC interface. The data is
    // cached only to avoid repeated marshalling; every use still validates the
    // untrusted ring pointers before touching host/kernel memory.
    CORO_TASK(int) controller::refresh_iouring_data()
    {
        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            clear_iouring_data_cache();
            CO_RETURN shutdown_err;
        }

        data ring_data;
        auto err = CO_AWAIT inner_get_iouring_data(ring_data);
        if (err != rpc::error::OK())
        {
            clear_iouring_data_cache();
            CO_RETURN err;
        }

        shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            clear_iouring_data_cache();
            CO_RETURN shutdown_err;
        }

        // The descriptor is still untrusted after it is cached. Caching only
        // avoids an expensive marshalled call on every operation; each ring
        // access still copies and validates data from host/kernel memory.
        {
            std::lock_guard<rpc::spin_mutex> lock(cache_mutex_);
            cached_iouring_data_ = ring_data;
            has_cached_iouring_data_ = true;
        }
        CO_RETURN rpc::error::OK();
    }

    // Drops any cached descriptor so a later operation is forced to ask the host
    // for a fresh view of the ring resources.
    void controller::clear_iouring_data_cache() noexcept
    {
        std::lock_guard<rpc::spin_mutex> lock(cache_mutex_);
        cached_iouring_data_ = {};
        has_cached_iouring_data_ = false;
    }

    // Exposes whether a descriptor is cached without performing an RPC. This is
    // useful for diagnostics, but callers must not treat the returned host data
    // as trusted.
    const data* controller::cached_iouring_data() const noexcept
    {
        return has_cached_iouring_data_ ? &cached_iouring_data_ : nullptr;
    }

    // Adds one completion-pump sample to the counters. A pump may have found
    // zero CQEs, which is still useful when comparing polling strategies.
    void controller::record_completion_pump(uint32_t completion_count) noexcept
    {
        measurements_.completion_pump_calls.fetch_add(1, std::memory_order_relaxed);
        measurements_.completion_entries.fetch_add(completion_count, std::memory_order_relaxed);
    }

    // Accounts for completions drained by an explicit CQ pump and resumes any
    // coroutine waiters that the operation engine detached from its table.
    void controller::consume_pump_result(detail::enclave_io_completion_pump_result& pump_result) noexcept
    {
        record_completion_pump(pump_result.completion_count);
        resume_completed_operations(std::move(pump_result.completed_operations));
    }

    // Accounts for completions opportunistically drained during submission; this
    // lets submitters make progress even when they are the only active pump.
    void controller::consume_submit_result(detail::enclave_io_submission_result& submit_result) noexcept
    {
        record_completion_pump(submit_result.completion_count);
        resume_completed_operations(std::move(submit_result.completed_operations));
    }

    // Resumes coroutines whose CQEs have been matched to enclave-owned operation
    // records. The linked list is produced while the operation engine lock is
    // held, then resumed outside that lock by this function.
    void controller::resume_completed_operations(detail::enclave_io_operation_engine::operation_ptr completed_operations) noexcept
    {
        while (completed_operations)
        {
            auto operation = std::move(completed_operations);
            completed_operations = std::move(operation->next_completed);
            operation->next_completed.reset();

            auto waiter = operation->waiter;
            operation->waiter = {};
            if (!waiter || waiter.done())
            {
                continue;
            }

            measurements_.proactor_resumes.fetch_add(1, std::memory_order_relaxed);
            if (scheduler_)
            {
                scheduler_->resume(waiter);
            }
            else
            {
                waiter.resume();
            }
        }
    }

    // Copies the cached descriptor under the spin mutex so operation paths can
    // validate and use a stable local snapshot without holding the cache lock.
    data controller::cached_iouring_data_copy() const noexcept
    {
        std::lock_guard<rpc::spin_mutex> lock(cache_mutex_);
        return cached_iouring_data_;
    }

    // Ensures this controller has a descriptor snapshot; this is the common
    // cheap fast path before every operation touches SQ/CQ memory.
    CORO_TASK(int) controller::ensure_iouring_data()
    {
        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            CO_RETURN shutdown_err;
        }

        {
            std::lock_guard<rpc::spin_mutex> lock(cache_mutex_);
            if (has_cached_iouring_data_)
            {
                CO_RETURN rpc::error::OK();
            }
        }

        CO_RETURN CO_AWAIT refresh_iouring_data();
    }

    // Verifies the host created a registered fixed-file table. TCP direct
    // descriptors rely on that table because the enclave never receives normal
    // process file descriptors.
    CORO_TASK(int) controller::ensure_fixed_file_table()
    {
        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN err;
        }

        const auto ring_data = cached_iouring_data_copy();
        if (ring_data.descriptor_version < 2 || !ring_data.fixed_files.fixed_files_registered
            || ring_data.fixed_files.fixed_file_count == 0)
        {
            RPC_WARNING(
                "enclave io_uring direct descriptor table unavailable descriptor_version={} fixed_registered={} "
                "fixed_count={}",
                ring_data.descriptor_version,
                ring_data.fixed_files.fixed_files_registered,
                ring_data.fixed_files.fixed_file_count);
            CO_RETURN rpc::error::PROTOCOL_ERROR();
        }

        CO_RETURN rpc::error::OK();
    }
} // namespace rpc::io_uring
