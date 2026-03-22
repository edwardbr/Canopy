/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "src/benchmark_common.h"

#include <fmt/format.h>

int main()
{
    using namespace comprehensive::v1;

    const std::vector<encoding_info> encodings = {
        {rpc::encoding::yas_binary, "yas_binary"},
        {rpc::encoding::yas_compressed_binary, "yas_compressed"},
        {rpc::encoding::protocol_buffers, "protocol_buffers"},
    };

    const std::vector<size_t> blob_sizes = {
        64,
        256,
        1024,
        4096,
        16384,
        65536,
        131072,
        262144,
        524288,
        1048576,
    };

    fmt::print("RPC++ Comprehensive Demo - Benchmark\n");
    fmt::print("====================================\n\n");
    print_header();

    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
#ifdef CANOPY_BUILD_COROUTINE
            auto scheduler = make_benchmark_scheduler();
            auto result = coro::sync_wait(run_local_benchmark(scheduler, enc.enc, blob_size));
#else
            auto result = run_local_benchmark(enc.enc, blob_size);
#endif
            if (result.error == rpc::error::OK())
                print_stats("local", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "local", enc.name, blob_size, result.error);
        }
    }

#ifndef CANOPY_BUILD_COROUTINE
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto result = run_dynamic_library_benchmark(enc.enc, blob_size);
            if (result.error == rpc::error::OK())
                print_stats("dll", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "dll", enc.name, blob_size, result.error);
        }
    }
#else
    fmt::print("run_libcoro_dynamic_library_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto scheduler = make_benchmark_scheduler();
            auto result = coro::sync_wait(run_libcoro_dynamic_library_benchmark(scheduler, enc.enc, blob_size));
            if (result.error == rpc::error::OK())
                print_stats("libcoro_dll", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "libcoro_dll", enc.name, blob_size, result.error);
        }
    }

    fmt::print("run_ipc_direct_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto scheduler = make_benchmark_scheduler();
            auto result = coro::sync_wait(run_ipc_direct_benchmark(scheduler, enc.enc, blob_size));
            if (result.error == rpc::error::OK())
                print_stats("ipc_direct", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "ipc_direct", enc.name, blob_size, result.error);
        }
    }

    fmt::print("run_ipc_dll_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto scheduler = make_benchmark_scheduler();
            auto result = coro::sync_wait(run_ipc_dll_benchmark(scheduler, enc.enc, blob_size));
            if (result.error == rpc::error::OK())
                print_stats("ipc_dll", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "ipc_dll", enc.name, blob_size, result.error);
        }
    }

    fmt::print("run_spsc_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto result = run_spsc_benchmark(enc.enc, blob_size);
            if (result.error == rpc::error::OK())
                print_stats("spsc", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "spsc", enc.name, blob_size, result.error);
        }
    }

    fmt::print("run_tcp_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto result = run_tcp_benchmark(enc.enc, blob_size, allocate_loopback_port());
            if (result.error == rpc::error::OK())
                print_stats("tcp", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "tcp", enc.name, blob_size, result.error);
        }
    }

    fmt::print("run_io_uring_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (const auto blob_size : blob_sizes)
        {
            auto result = run_io_uring_benchmark(enc.enc, blob_size, allocate_loopback_port());
            if (result.error == rpc::error::OK())
                print_stats("io_uring", enc.name, blob_size, result.stats);
            else
                fmt::print("{:>10} | {:>18} | {:>9} | error {}\n", "io_uring", enc.name, blob_size, result.error);
        }
    }
#endif

    print_footer();
    return 0;
}
