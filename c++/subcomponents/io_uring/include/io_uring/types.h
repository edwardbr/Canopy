/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>

#include <rpc/rpc.h>

namespace rpc::io_uring
{
    enum class wait_strategy : uint32_t
    {
        cooperative_poll = 0,
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
