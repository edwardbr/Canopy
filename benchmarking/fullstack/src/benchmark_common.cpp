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

#ifdef CANOPY_BUILD_COROUTINE
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace comprehensive::v1
{
    std::vector<uint8_t> make_blob(size_t size)
    {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<uint8_t>(i % 251);
        return data;
    }

    benchmark_stats compute_stats(const std::vector<int64_t>& samples)
    {
        benchmark_stats stats{};
        if (samples.size() < (trim_each_side * 2))
            return stats;

        const size_t begin = trim_each_side;
        const size_t end = samples.size() - trim_each_side;
        std::vector<int64_t> mid(samples.begin() + static_cast<long>(begin), samples.begin() + static_cast<long>(end));

        std::sort(mid.begin(), mid.end());
        const size_t mid_count = mid.size();
        if (mid_count == 0)
            return stats;

        const auto sum = std::accumulate(mid.begin(), mid.end(), int64_t{0});
        stats.avg_us = static_cast<double>(sum) / static_cast<double>(mid_count);
        stats.min_us = static_cast<double>(mid.front());
        stats.max_us = static_cast<double>(mid.back());
        stats.p50_us = static_cast<double>(mid[(mid_count * 50) / 100]);
        stats.p90_us = static_cast<double>(mid[(mid_count * 90) / 100]);
        stats.p95_us = static_cast<double>(mid[(mid_count * 95) / 100]);
        return stats;
    }

    void print_stats(const char* transport, const char* encoding, size_t blob_size, const benchmark_stats& stats)
    {
        const double size_mb = static_cast<double>(blob_size) / (1024.0 * 1024.0);
        constexpr double min_time_us = 0.5;
        const bool throughput_valid = (stats.avg_us >= min_time_us);

        if (throughput_valid)
        {
            const double payload_mb_per_sec = size_mb / (stats.avg_us / 1e6);
            const double round_trip_mb_per_sec = (size_mb * 2.0) / (stats.avg_us / 1e6);
            fmt::print("{:>10} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
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

    CORO_TASK(int)
    run_benchmark_calls(rpc::shared_ptr<i_data_processor> remote,
        const std::vector<uint8_t>& payload,
        std::vector<int64_t>& durations_us,
        size_t warmup_count)
    {
        durations_us.clear();
        durations_us.reserve(call_count);

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
                CO_RETURN rpc::error::INVALID_DATA();

            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            durations_us.push_back(static_cast<int64_t>(elapsed));
        }

        CO_RETURN rpc::error::OK();
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
#endif

    void print_header()
    {
        fmt::print("Benchmark: 1000 RPC calls per test, middle 80% (drop first/last 10%)\n");
#ifdef CANOPY_BUILD_COROUTINE
        fmt::print("Warmup: local=10 calls, libcoro_dll=20 calls, ipc=30 calls, spsc=20 calls, io_uring=100 calls, "
                   "tcp=100 calls (not included in timing)\n");
#else
        fmt::print("Warmup: local=10 calls, dynamic_library=20 calls (not included in timing)\n");
#endif
        fmt::print("Note: Throughput shown as 'N/A' when avg time < 0.5us (insufficient timing precision)\n");
        fmt::print("Units: MB/s = megabytes per second (1 MB = 1024*1024 bytes)\n");
        fmt::print("---------------------------------------------------------------------------------------------------"
                   "---------"
                   "----------------\n");
        fmt::print(
            "transport   | serialization       | blob bytes | avg (us)     | p50       | p90       | p95       | "
            "min       | max       | payload MB/s | round-trip MB/s\n");
        fmt::print("---------------------------------------------------------------------------------------------------"
                   "---------"
                   "----------------\n");
    }

    void print_footer()
    {
        fmt::print("---------------------------------------------------------------------------------------------------"
                   "---------"
                   "----------------\n");
    }
}
