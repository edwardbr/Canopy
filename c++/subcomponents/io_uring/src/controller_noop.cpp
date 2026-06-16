/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <atomic>
#include <exception>
#include <new>

namespace rpc::io_uring
{
    namespace
    {
        // Reads the local cycle counter for relative no_op measurements. This
        // is diagnostic only; it is not trusted wall-clock time.
        uint64_t read_tick_counter() noexcept
        {
#if defined(__x86_64__) || defined(__i386__)
            return __builtin_ia32_rdtsc();
#else
            return 0;
#endif
        }

        // Converts a saved cycle-counter value into a non-negative delta while
        // tolerating unsupported architectures that return zero.
        uint64_t elapsed_ticks(uint64_t start_ticks) noexcept
        {
            if (start_ticks == 0)
            {
                return 0;
            }

            auto end_ticks = read_tick_counter();
            return end_ticks >= start_ticks ? end_ticks - start_ticks : 0;
        }

        // Updates a maximum counter without taking a lock; losing races retry
        // until the stored max is at least the candidate value.
        void atomic_update_max(
            std::atomic<uint64_t>& target,
            uint64_t value) noexcept
        {
            auto current = target.load(std::memory_order_relaxed);
            while (current < value
                   && !target.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed))
            {
            }
        }
    } // namespace

    // Submits a single IORING_OP_NOP through the direct-ring path. This is the
    // simplest end-to-end proof that the controller can validate ring data,
    // publish an SQE, optionally wake the environment, and observe a matching
    // CQE.
    CORO_TASK(int) controller::no_op()
    {
        measurements_.no_op_calls.fetch_add(1, std::memory_order_relaxed);

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            record_no_op_complete(0, err);
            CO_RETURN err;
        }

        const auto ring_data = cached_iouring_data_copy();
        if (!detail::validate_ring_data_for_direct_ring(ring_data))
        {
            RPC_WARNING(
                "direct io_uring no_op invalid ring data descriptor_version={} setup_flags={} sq_entries={} "
                "cq_entries={} sq_array_ptr={}",
                ring_data.descriptor_version,
                ring_data.setup.setup_flags,
                ring_data.submission_queue.sq_ring_entries,
                ring_data.completion_queue.cq_ring_entries,
                ring_data.submission_queue.sq_array_ptr);
            record_no_op_complete(0, rpc::error::PROTOCOL_ERROR());
            CO_RETURN rpc::error::PROTOCOL_ERROR();
        }

        const auto start_ticks = read_tick_counter();
        std::shared_ptr<detail::direct_ring_operation> operation;
        try
        {
            operation = std::make_shared<detail::direct_ring_operation>();
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring no_op");
            std::terminate();
        }

        err = CO_AWAIT submit_no_op(ring_data, operation);
        if (err != rpc::error::OK())
        {
            record_no_op_complete(start_ticks, err);
            CO_RETURN err;
        }

        // SQPOLL only needs an explicit wake when the kernel requests it. A
        // non-SQPOLL ring needs io_uring_enter() for every submitted SQE.
        if (detail::submission_notification_needed(ring_data))
        {
            err = CO_AWAIT notify_submitted(ring_data, 1);
            if (err != rpc::error::OK())
            {
                resume_completed_operations(operation_engine_.fail_all_operations(err));
                RPC_WARNING("direct io_uring no_op wake_iouring failed error={}", err);
                record_no_op_complete(start_ticks, err);
                CO_RETURN err;
            }
        }

        err = CO_AWAIT wait_for_operation(ring_data, operation);
        record_no_op_complete(start_ticks, err);
        CO_RETURN err;
    }

    // Records no_op success/failure and latency counters in one place so all
    // no_op exit paths keep the measurement data consistent.
    void controller::record_no_op_complete(
        uint64_t start_ticks,
        int err) noexcept
    {
        if (err == rpc::error::OK())
        {
            measurements_.no_op_successes.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            measurements_.no_op_failures.fetch_add(1, std::memory_order_relaxed);
        }

        const auto ticks = elapsed_ticks(start_ticks);
        measurements_.total_no_op_ticks.fetch_add(ticks, std::memory_order_relaxed);
        atomic_update_max(measurements_.max_no_op_ticks, ticks);
    }
} // namespace rpc::io_uring
