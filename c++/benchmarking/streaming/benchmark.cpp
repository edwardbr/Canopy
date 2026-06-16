/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <fmt/format.h>

#include <iostream>

namespace
{
    template<class Job>
    void print_sample_progress(
        const char* category,
        const stream_bench::bench_config& cfg,
        size_t index,
        size_t total,
        const stream_bench::benchmark_execution& execution,
        const Job& job)
    {
        if (cfg.passes <= 1)
            return;

        fmt::print(
            "{} sample {}/{} pass {}/{} {} | {}\n",
            category,
            index + 1,
            total,
            execution.pass + 1,
            cfg.passes,
            job.stream,
            job.blob_size);
    }
}

int main(
    int argc,
    char** argv)
{
    using namespace stream_bench;

    bench_config cfg;
    const auto parse_result = parse_args(argc, argv, cfg);
    if (parse_result == parse_status::help)
        return 0;
    if (parse_result == parse_status::error)
        return 1;

    std::cout << "Canopy Streaming Benchmark\n";
    std::cout << "=========================\n\n";
    print_configuration(cfg);

    if (cfg.watchdog_timeout.count() > 0 && cfg.recv_timeout > cfg.watchdog_timeout)
    {
        fmt::print(
            stderr,
            "WARNING: --timeout-ms ({}) > --watchdog-ms ({}). A slow receive can trip the watchdog first.\n\n",
            cfg.recv_timeout.count(),
            cfg.watchdog_timeout.count());
    }

    watchdog wd{cfg.watchdog_timeout};
    wd.heartbeat();

    std::vector<standard_benchmark_job> standard_jobs;
    std::vector<stress_benchmark_job> stress_jobs;
#ifndef CANOPY_BUILD_COROUTINE
    add_tcp_blocking_jobs(cfg, wd, standard_jobs, stress_jobs);
#  ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
    add_tls_tcp_jobs(cfg, wd, standard_jobs, stress_jobs);
#  endif
#  ifdef CANOPY_BUILD_WEBSOCKET
    add_websocket_tcp_jobs(cfg, wd, standard_jobs, stress_jobs);
#    ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
    add_tls_websocket_tcp_jobs(cfg, wd, standard_jobs, stress_jobs);
#    endif
#  endif
#endif

#ifdef CANOPY_BUILD_COROUTINE
    add_spsc_jobs(cfg, wd, standard_jobs, stress_jobs);
#  ifdef CANOPY_STREAMING_BENCHMARK_HAS_TCP_COROUTINE
    add_tcp_coroutine_jobs(cfg, wd, standard_jobs, stress_jobs);
#  endif
#  ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
    add_tls_spsc_jobs(cfg, wd, standard_jobs, stress_jobs);
#  endif
#  ifdef CANOPY_BUILD_WEBSOCKET
    add_websocket_spsc_jobs(cfg, wd, standard_jobs, stress_jobs);
#    ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
    add_tls_websocket_spsc_jobs(cfg, wd, standard_jobs, stress_jobs);
#    endif
#  endif
#endif

    if (standard_jobs.empty() && stress_jobs.empty())
    {
        fmt::print(stderr, "No streaming benchmark entries matched the selected filters.\n");
        print_usage(argv[0]);
        return 1;
    }

    std::vector<standard_result_sample> standard_samples;
    if (!standard_jobs.empty())
    {
        const auto executions = make_execution_order(standard_jobs.size(), cfg);
        standard_samples.reserve(executions.size());
        for (size_t index = 0; index < executions.size(); ++index)
        {
            const auto execution = executions[index];
            const auto& job = standard_jobs[execution.job_index];
            print_sample_progress("standard", cfg, index, executions.size(), execution, job);
            wd.set_context(fmt::format("{} blob={}B", job.stream, job.blob_size));
            wd.heartbeat();

            standard_result_sample sample;
            sample.stream = job.stream;
            sample.blob_size = job.blob_size;
            job.run(sample.unidirectional, sample.send_reply);
            standard_samples.push_back(std::move(sample));
        }
    }

    std::vector<stress_result_sample> stress_samples;
    if (!stress_jobs.empty())
    {
        const auto executions = make_execution_order(stress_jobs.size(), cfg);
        stress_samples.reserve(executions.size());
        for (size_t index = 0; index < executions.size(); ++index)
        {
            const auto execution = executions[index];
            const auto& job = stress_jobs[execution.job_index];
            print_sample_progress("stress", cfg, index, executions.size(), execution, job);
            wd.set_context(fmt::format("{} stress blob={}B", job.stream, job.blob_size));
            wd.heartbeat();

            stress_result_sample sample;
            sample.stream = job.stream;
            sample.blob_size = job.blob_size;
            job.run(sample.send, sample.recv);
            stress_samples.push_back(std::move(sample));
        }
    }

    const auto unidirectional_rows = cfg.run_unidirectional ? aggregate_standard_samples(standard_samples, true)
                                                            : std::vector<standard_result_row>{};
    const auto send_reply_rows
        = cfg.run_send_reply ? aggregate_standard_samples(standard_samples, false) : std::vector<standard_result_row>{};
    const auto stress_rows = cfg.run_stress ? aggregate_stress_samples(stress_samples) : std::vector<stress_result_row>{};

    if (cfg.run_unidirectional)
    {
        print_unidirectional_header(cfg);
        for (const auto& row : unidirectional_rows)
            print_unidirectional_row(row);
    }

    if (cfg.run_send_reply)
    {
        print_send_reply_header(cfg);
        for (const auto& row : send_reply_rows)
            print_send_reply_row(row);
    }

    if (cfg.run_stress)
    {
        print_stress_header(cfg);
        for (const auto& row : stress_rows)
            print_stress_row(row);
    }

    if (cfg.passes > 1)
    {
        fmt::print("\nAggregate quality summary\n");
        for (const auto& row : unidirectional_rows)
            print_standard_quality_row("unidirectional", row);
        for (const auto& row : send_reply_rows)
            print_standard_quality_row("send_reply", row);
        for (const auto& row : stress_rows)
            print_stress_quality_row(row);
    }

    if (cfg.write_html_report)
    {
        if (!write_html_report(cfg.html_report_path, unidirectional_rows, send_reply_rows, stress_rows))
            return 1;
        fmt::print("\nHTML streaming benchmark report written to {}\n", cfg.html_report_path.string());
    }

    fmt::print("\nDone.\n");
    return 0;
}
