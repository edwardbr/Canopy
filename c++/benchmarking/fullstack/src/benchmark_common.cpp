/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

#include <fmt/format.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace comprehensive::v1
{
    std::vector<uint8_t> make_blob(size_t size)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<uint8_t>(i % 251);
        return data;
    }

    benchmark_stats compute_stats(const std::vector<int64_t>& samples_ns)
    {
        benchmark_stats stats{};
        if (samples_ns.size() < (trim_each_side * 2))
            return stats;

        std::vector<int64_t> sorted(samples_ns.begin(), samples_ns.end());
        std::sort(sorted.begin(), sorted.end());

        const size_t begin = trim_each_side;
        const size_t end = samples_ns.size() - trim_each_side;
        const size_t mid_count = end - begin;
        if (mid_count == 0)
            return stats;

        const auto mid_begin = sorted.begin() + static_cast<long>(begin);
        const auto mid_end = sorted.begin() + static_cast<long>(end);
        const auto sum = std::accumulate(mid_begin, mid_end, int64_t{0});
        constexpr double ns_to_us = 1.0 / 1000.0;
        stats.avg_us = (static_cast<double>(sum) / static_cast<double>(mid_count)) * ns_to_us;
        stats.min_us = static_cast<double>(*mid_begin) * ns_to_us;
        stats.max_us = static_cast<double>(*(mid_end - 1)) * ns_to_us;
        stats.p50_us = static_cast<double>(*(mid_begin + static_cast<long>((mid_count * 50) / 100))) * ns_to_us;
        stats.p90_us = static_cast<double>(*(mid_begin + static_cast<long>((mid_count * 90) / 100))) * ns_to_us;
        stats.p95_us = static_cast<double>(*(mid_begin + static_cast<long>((mid_count * 95) / 100))) * ns_to_us;
        return stats;
    }

    void print_stats(
        const char* transport,
        const char* encoding,
        size_t blob_size,
        const benchmark_stats& stats)
    {
        const double size_mb = static_cast<double>(blob_size) / (1024.0 * 1024.0);
        const bool throughput_valid = (stats.avg_us > 0.0);

        if (throughput_valid)
        {
            const double payload_mb_per_sec = size_mb / (stats.avg_us / 1e6);
            const double round_trip_mb_per_sec = (size_mb * 2.0) / (stats.avg_us / 1e6);
            fmt::print(
                "{:>10} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
                "{:>8.2f} | max {:>8.2f} | payload {:>7.2f} MB/s | round-trip {:>7.2f} MB/s\n",
                transport,
                encoding,
                blob_size,
                stats.avg_us,
                stats.p50_us,
                stats.p90_us,
                stats.p95_us,
                stats.min_us,
                stats.max_us,
                payload_mb_per_sec,
                round_trip_mb_per_sec);
            return;
        }

        fmt::print(
            "{:>10} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min {:>8.2f} "
            "| max {:>8.2f} | payload {:>7} MB/s | round-trip {:>7} MB/s\n",
            transport,
            encoding,
            blob_size,
            stats.avg_us,
            stats.p50_us,
            stats.p90_us,
            stats.p95_us,
            stats.min_us,
            stats.max_us,
            "N/A",
            "N/A");
    }

    CORO_TASK(comprehensive_error)
    run_benchmark_calls(
        rpc::shared_ptr<i_data_processor> remote,
        const std::vector<uint8_t>& payload,
        std::vector<int64_t>& durations_ns,
        size_t warmup_count)
    {
        durations_ns.clear();
        durations_ns.reserve(call_count);

        std::vector<uint8_t> response;

        for (size_t i = 0; i < warmup_count; ++i)
        {
            const auto error = CO_AWAIT remote->echo_binary(payload, response);
            if (error != rpc::error::OK())
                CO_RETURN error;
        }

        for (size_t i = 0; i < call_count; ++i)
        {
            const auto start = clock_type::now();
            const auto error = CO_AWAIT remote->echo_binary(payload, response);
            const auto end = clock_type::now();

            if (error != rpc::error::OK())
                CO_RETURN error;

            if (response.size() != payload.size())
            {
                CO_RETURN comprehensive_error::INVALID_BENCHMARK_RESULT;
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            durations_ns.push_back(static_cast<int64_t>(elapsed));
        }

        CO_RETURN rpc::error::OK();
    }

    uint16_t allocate_loopback_port()
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        RPC_ASSERT(fd >= 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        const int bind_result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        RPC_ASSERT(bind_result == 0);

        sockaddr_in bound_addr{};
        socklen_t bound_addr_len = sizeof(bound_addr);
        const int getsockname_result = ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_addr_len);
        RPC_ASSERT(getsockname_result == 0);

        ::close(fd);
        return ntohs(bound_addr.sin_port);
    }

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> make_benchmark_scheduler(uint32_t thread_count)
    {
        return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
            coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                .pool = coro::thread_pool::options{.thread_count = thread_count},
                .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
    }

    void wait_for_scheduler_cleanup(std::weak_ptr<coro::scheduler> scheduler)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!scheduler.expired() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (!scheduler.expired())
            std::cerr << "benchmark: scheduler cleanup timed out\n";
    }
#endif

    void print_header()
    {
        fmt::print(
            "Benchmark: {} RPC calls per test, sorted middle {} samples (drop fastest/slowest {} each)\n",
            call_count,
            call_count - (trim_each_side * 2),
            trim_each_side);
#ifdef CANOPY_BUILD_COROUTINE
        fmt::print(
            "Warmup: local={} calls, unshared_scheduler_dll={} calls, shared_scheduler_dll={} calls, ipc={} calls, "
            "spsc={} calls, "
            "io_uring={} calls, tcp={} calls, sgx_io_uring={} calls, sgx_io_uring_pair={} calls "
            "(not included in timing)\n",
            local_warmup_calls,
            dll_warmup_calls,
            dll_warmup_calls,
            ipc_warmup_calls,
            spsc_warmup_calls,
            io_uring_warmup_calls,
            tcp_warmup_calls,
            io_uring_warmup_calls,
            io_uring_warmup_calls);
#else
        fmt::print(
            "Warmup: local={} calls, blocking_dll={} calls, tcp={} calls (not included in timing)\n",
            local_warmup_calls,
            dll_warmup_calls,
            tcp_warmup_calls);
#endif
        fmt::print("Note: Timings are sampled in nanoseconds and displayed in microseconds\n");
        fmt::print("Units: MB/s = megabytes per second (1 MB = 1024*1024 bytes)\n");
        fmt::print(
            "---------------------------------------------------------------------------------------------------"
            "---------"
            "----------------\n");
        fmt::print(
            "transport   | serialization       | blob bytes | avg (us)     | p50       | p90       | p95       | "
            "min       | max       | payload MB/s | round-trip MB/s\n");
        fmt::print(
            "---------------------------------------------------------------------------------------------------"
            "---------"
            "----------------\n");
    }

    void print_footer()
    {
        fmt::print(
            "---------------------------------------------------------------------------------------------------"
            "---------"
            "----------------\n");
    }
}
