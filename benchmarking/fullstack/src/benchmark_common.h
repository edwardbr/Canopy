/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/rpc.h>
#include <comprehensive/comprehensive.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace comprehensive::v1
{
    using clock_type = std::chrono::steady_clock;

    inline constexpr bool reduced_debug_benchmark_matrix =
#if defined(NDEBUG)
        false;
#else
        true;
#endif

    inline constexpr size_t call_count = reduced_debug_benchmark_matrix ? 5 : 1000;
    inline constexpr size_t trim_each_side = call_count / 10;
    inline constexpr size_t local_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 10;
    inline constexpr size_t dll_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 20;
    inline constexpr size_t ipc_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 30;
    inline constexpr size_t spsc_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 20;
    inline constexpr size_t tcp_warmup_calls = reduced_debug_benchmark_matrix ? 2 : 100;
    inline constexpr size_t io_uring_warmup_calls = reduced_debug_benchmark_matrix ? 2 : 100;

    struct benchmark_stats
    {
        double avg_us = 0.0;
        double min_us = 0.0;
        double max_us = 0.0;
        double p50_us = 0.0;
        double p90_us = 0.0;
        double p95_us = 0.0;
    };

    struct benchmark_result
    {
        int error = rpc::error::OK();
        benchmark_stats stats{};
    };

    struct encoding_info
    {
        rpc::encoding enc;
        const char* name;
    };

    std::vector<uint8_t> make_blob(size_t size);
    benchmark_stats compute_stats(const std::vector<int64_t>& samples);
    void print_stats(
        const char* transport,
        const char* encoding,
        size_t blob_size,
        const benchmark_stats& stats);
    CORO_TASK(int)
    run_benchmark_calls(
        rpc::shared_ptr<i_data_processor> remote,
        const std::vector<uint8_t>& payload,
        std::vector<int64_t>& durations_us,
        size_t warmup_count = 0);

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> make_benchmark_scheduler(uint32_t thread_count = 2);
    void wait_for_scheduler_cleanup(std::weak_ptr<coro::scheduler> scheduler);
    uint16_t allocate_loopback_port();
#endif

#ifdef CANOPY_BUILD_COROUTINE
    CORO_TASK(benchmark_result)
    run_local_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size);
    CORO_TASK(benchmark_result)
    run_libcoro_dynamic_library_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size);
    CORO_TASK(benchmark_result)
    run_ipc_direct_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size);
    CORO_TASK(benchmark_result)
    run_ipc_dll_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size);
    benchmark_result run_spsc_benchmark(
        rpc::encoding enc,
        size_t blob_size);
    benchmark_result run_tcp_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port);
    benchmark_result run_io_uring_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        uint16_t port);
#else
    CORO_TASK(benchmark_result)
    run_local_benchmark(
        rpc::encoding enc,
        size_t blob_size);
    CORO_TASK(benchmark_result)
    run_dynamic_library_benchmark(
        rpc::encoding enc,
        size_t blob_size);
#endif

    void print_header();
    void print_footer();
}
