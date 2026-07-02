/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <rpc/rpc.h>
#include <streaming/stream.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <coro/coro.hpp>
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace stream_bench
{
    using clock_type = std::chrono::steady_clock;

    struct bench_config
    {
        std::set<std::string> streams;
        bool run_unidirectional = true;
        bool run_send_reply = true;
        bool run_stress = false;
        size_t count = 1000;
        size_t warmup = 20;
        std::vector<size_t> sizes;
        std::chrono::milliseconds recv_timeout{1000};
        std::chrono::seconds stress_duration{30};
        std::chrono::milliseconds watchdog_timeout{10000};
        size_t passes = 1;
        bool shuffle = false;
        uint32_t shuffle_seed = 0xc0ffeeu;
        bool write_html_report = true;
        std::filesystem::path html_report_path;
    };

    enum class parse_status
    {
        ok,
        help,
        error
    };

    struct bench_stats
    {
        double avg = 0.0;
        double min = 0.0;
        double max = 0.0;
        double p50 = 0.0;
        double p90 = 0.0;
        double p95 = 0.0;
        size_t blob_size = 0;
        bool valid = false;
    };

    struct stress_stats
    {
        uint64_t ops_sent = 0;
        uint64_t bytes_sent = 0;
        uint64_t ops_recvd = 0;
        uint64_t bytes_recvd = 0;
        uint64_t recv_timeouts = 0;
        double elapsed_ms = 0.0;
        size_t blob_size = 0;
        bool valid = false;

        double send_mbps() const;
        double recv_mbps() const;
    };

    struct standard_result_sample
    {
        std::string stream;
        size_t blob_size = 0;
        bench_stats unidirectional{};
        bench_stats send_reply{};
    };

    struct standard_result_row
    {
        std::string stream;
        size_t blob_size = 0;
        bench_stats stats{};
        size_t samples = 0;
        size_t failures = 0;
        double avg_mean = 0.0;
        double avg_stddev = 0.0;
    };

    struct stress_result_sample
    {
        std::string stream;
        size_t blob_size = 0;
        stress_stats send{};
        stress_stats recv{};
    };

    struct stress_result_row
    {
        std::string stream;
        size_t blob_size = 0;
        stress_stats send{};
        stress_stats recv{};
        size_t samples = 0;
        size_t failures = 0;
        double send_mbps_mean = 0.0;
        double send_mbps_stddev = 0.0;
        double recv_mbps_mean = 0.0;
        double recv_mbps_stddev = 0.0;
    };

    struct benchmark_execution
    {
        size_t job_index = 0;
        size_t pass = 0;
    };

    struct standard_benchmark_job
    {
        std::string stream;
        size_t blob_size = 0;
        std::function<void(bench_stats&, bench_stats&)> run;
    };

    struct stress_benchmark_job
    {
        std::string stream;
        size_t blob_size = 0;
        std::function<void(stress_stats&, stress_stats&)> run;
    };

    class watchdog
    {
    public:
        explicit watchdog(std::chrono::milliseconds timeout);
        ~watchdog();

        watchdog(const watchdog&) = delete;
        auto operator=(const watchdog&) -> watchdog& = delete;

        void heartbeat();
        void set_context(const std::string& context);

    private:
        static int64_t now_ns();
        void run();

        std::chrono::milliseconds timeout_;
        std::atomic<int64_t> last_ns_;
        std::atomic<bool> stop_;
        std::thread thread_;
        std::mutex context_mutex_;
        std::string context_;
    };

    std::filesystem::path default_html_report_path();
    parse_status parse_args(
        int argc,
        char** argv,
        bench_config& cfg);
    void print_usage(const char* program);
    void print_configuration(const bench_config& cfg);

    bool should_run_stream(
        const bench_config& cfg,
        std::string_view stream);
    std::vector<size_t> get_blob_sizes(const bench_config& cfg);
    std::vector<size_t> get_stress_blob_sizes(const bench_config& cfg);
    std::vector<benchmark_execution> make_execution_order(
        size_t job_count,
        const bench_config& cfg);

#ifdef CANOPY_BUILD_COROUTINE
    std::shared_ptr<coro::scheduler> make_scheduler();
#endif
    uint16_t allocate_loopback_port();

#ifndef CANOPY_BUILD_COROUTINE
    struct tcp_stream_pair
    {
        std::shared_ptr<streaming::stream> side_a;
        std::shared_ptr<streaming::stream> side_b;

        void shutdown();
    };

    bool make_tcp_stream_pair(tcp_stream_pair& pair);
#endif

    bench_stats compute_stats(
        std::vector<int64_t> samples,
        size_t blob_size,
        size_t trim_count);
    std::vector<standard_result_row> aggregate_standard_samples(
        const std::vector<standard_result_sample>& samples,
        bool use_unidirectional);
    std::vector<stress_result_row> aggregate_stress_samples(const std::vector<stress_result_sample>& samples);

    void print_unidirectional_header(const bench_config& cfg);
    void print_unidirectional_row(const standard_result_row& row);
    void print_send_reply_header(const bench_config& cfg);
    void print_send_reply_row(const standard_result_row& row);
    void print_stress_header(const bench_config& cfg);
    void print_stress_row(const stress_result_row& row);
    void print_standard_quality_row(
        const char* scenario,
        const standard_result_row& row);
    void print_stress_quality_row(const stress_result_row& row);

    bool write_html_report(
        const std::filesystem::path& report_path,
        const std::vector<standard_result_row>& unidirectional_rows,
        const std::vector<standard_result_row>& send_reply_rows,
        const std::vector<stress_result_row>& stress_rows);

    // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters):
    // benchmark coroutine callers join before these required context objects leave scope.
    CORO_TASK(bench_stats)
    run_unidirectional_sender(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd);
    CORO_TASK(void)
    run_drain(
        std::shared_ptr<streaming::stream> stream,
        std::atomic<bool>& stop,
        watchdog& wd);
    CORO_TASK(bench_stats)
    run_send_reply(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd);
    CORO_TASK(void)
    run_echo(
        std::shared_ptr<streaming::stream> stream,
        std::atomic<bool>& stop,
        watchdog& wd);
    CORO_TASK(stress_stats)
    run_stress_sender(
        std::shared_ptr<streaming::stream> stream,
        const std::vector<uint8_t>& payload,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd);
    CORO_TASK(stress_stats)
    run_stress_drain(
        std::shared_ptr<streaming::stream> stream,
        std::atomic<bool>& stop,
        const bench_config& cfg,
        watchdog& wd);
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

    void run_stream_unidirectional_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        bench_stats& out_unidirectional);
    void run_stream_send_reply_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        bench_stats& out_send_reply);
    void run_paired_stream_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        bench_stats& out_unidirectional,
        bench_stats& out_send_reply);
    void run_paired_stream_stress_bench(
        std::shared_ptr<streaming::stream> side_a,
        std::shared_ptr<streaming::stream> side_b,
        const bench_config& cfg,
        watchdog& wd,
        size_t blob_size,
        stress_stats& out_send,
        stress_stats& out_recv);

    void add_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
#ifndef CANOPY_BUILD_COROUTINE
    void add_tcp_blocking_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
#endif
    void add_tcp_coroutine_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
    void add_tls_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
    void add_websocket_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
    void add_tls_websocket_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
#ifndef CANOPY_BUILD_COROUTINE
    void add_tls_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
    void add_websocket_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
    void add_tls_websocket_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs);
#endif
}
