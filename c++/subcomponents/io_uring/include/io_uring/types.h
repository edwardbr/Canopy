/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rpc/rpc.h>

namespace rpc::io_uring
{
    struct fd_data
    {
        uint32_t has_ring_fd{0};
        int32_t ring_fd{0};
        uint32_t has_enter_ring_fd{0};
        int32_t enter_ring_fd{0};
    };

    struct setup_data
    {
        uint32_t setup_flags{0};
        uint32_t features{0};
        uint32_t sq_thread_idle_ms{0};
        uint32_t sq_entries{0};
        uint32_t cq_entries{0};
    };

    struct sq_data
    {
        uint32_t sq_ring_mask{0};
        uint32_t sq_ring_entries{0};

        uint32_t sq_off_head{0};
        uint32_t sq_off_tail{0};
        uint32_t sq_off_ring_mask{0};
        uint32_t sq_off_ring_entries{0};
        uint32_t sq_off_flags{0};
        uint32_t sq_off_dropped{0};
        uint32_t sq_off_array{0};

        uint64_t sq_ring_ptr{0};
        uint64_t sq_ring_size{0};
        uint64_t sq_head_ptr{0};
        uint64_t sq_tail_ptr{0};
        uint64_t sq_flags_ptr{0};
        uint64_t sq_dropped_ptr{0};
        uint64_t sq_array_ptr{0};
        uint64_t sqes_ptr{0};
    };

    struct cq_data
    {
        uint32_t cq_ring_mask{0};
        uint32_t cq_ring_entries{0};

        uint32_t cq_off_head{0};
        uint32_t cq_off_tail{0};
        uint32_t cq_off_ring_mask{0};
        uint32_t cq_off_ring_entries{0};
        uint32_t cq_off_overflow{0};
        uint32_t cq_off_cqes{0};
        uint32_t cq_off_flags{0};

        uint64_t cq_ring_ptr{0};
        uint64_t cq_ring_size{0};
        uint64_t cq_head_ptr{0};
        uint64_t cq_tail_ptr{0};
        uint64_t cq_flags_ptr{0};
        uint64_t cq_overflow_ptr{0};
        uint64_t cqes_ptr{0};
    };

    struct buffer_data
    {
        uint64_t buffer_region_ptr{0};
        uint64_t buffer_region_size{0};
        uint32_t buffer_size{0};
        uint32_t buffer_count{0};
        uint32_t buffers_registered{0};
        uint32_t registered_buffer_count{0};
    };

    struct fixed_file_data
    {
        uint32_t fixed_files_registered{0};
        uint32_t fixed_file_count{0};
    };

    struct data
    {
        uint32_t descriptor_version{2};
        fd_data file_descriptors;
        setup_data setup;
        sq_data submission_queue;
        cq_data completion_queue;
        buffer_data buffers;
        fixed_file_data fixed_files;
    };

    static_assert(sizeof(fd_data) == 16);
    static_assert(sizeof(setup_data) == 20);
    static_assert(sizeof(sq_data) == 104);
    static_assert(sizeof(cq_data) == 96);
    static_assert(sizeof(buffer_data) == 32);
    static_assert(sizeof(fixed_file_data) == 8);
    static_assert(sizeof(data) == 280);
    static_assert(alignof(data) == alignof(uint64_t));
    static_assert(sizeof(size_t) == sizeof(uint64_t));
    static_assert(sizeof(std::uintptr_t) == sizeof(uint64_t));

    enum class wait_strategy : uint32_t
    {
        // The coroutine waiting for an operation also pumps the completion
        // queue. This is the most self-contained mode: no background task is
        // needed, and progress happens whenever any waiter reaches its polling
        // loop. Recommended default for most applications.
        cooperative_poll = 0,
        // A scheduler-owned completion pump drains the completion queue and
        // resumes the coroutine registered on the completed operation. This
        // centralizes CQ polling for many concurrent operations, but requires a
        // scheduler; the controller falls back to cooperative polling when no
        // pump can be started. Prefer this only for workloads with enough
        // concurrent direct io_uring operations to benefit from one shared pump.
        proactor = 1
    };

    struct controller_measurements
    {
        uint64_t no_op_calls{0};
        uint64_t no_op_successes{0};
        uint64_t no_op_failures{0};
        uint64_t submit_attempts{0};
        uint64_t submit_backpressure{0};
        uint64_t completion_pump_calls{0};
        uint64_t completion_entries{0};
        uint64_t scheduler_yields{0};
        uint64_t local_relax_spins{0};
        uint64_t host_wake_calls{0};
        uint64_t proactor_pump_starts{0};
        uint64_t proactor_pump_iterations{0};
        uint64_t proactor_waiter_suspends{0};
        uint64_t proactor_resumes{0};
        uint64_t proactor_start_failures{0};
        // Raw local tick counter deltas for the direct io_uring operation path.
        // These are useful for comparisons within one run; they are not trusted
        // wall-clock time inside an enclave.
        uint64_t total_no_op_ticks{0};
        uint64_t max_no_op_ticks{0};
    };

    struct operation_result
    {
        int error_code{rpc::error::OK()};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
    };

    struct descriptor_result
    {
        int error_code{rpc::error::OK()};
        uint32_t descriptor{0};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
    };

    struct transfer_result
    {
        int error_code{rpc::error::OK()};
        uint32_t bytes_transferred{0};
        int32_t native_result{0};
        uint32_t cqe_flags{0};
    };
} // namespace rpc::io_uring
