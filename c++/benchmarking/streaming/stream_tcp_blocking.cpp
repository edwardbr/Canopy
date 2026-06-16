/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

namespace stream_bench
{
    namespace
    {
        void run_standard_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& out_unidirectional,
            bench_stats& out_send_reply)
        {
            if (cfg.run_unidirectional)
            {
                tcp_stream_pair pair;
                if (make_tcp_stream_pair(pair))
                    run_stream_unidirectional_bench(pair.side_a, pair.side_b, cfg, wd, blob_size, out_unidirectional);
                pair.shutdown();
            }

            if (cfg.run_send_reply)
            {
                tcp_stream_pair pair;
                if (make_tcp_stream_pair(pair))
                    run_stream_send_reply_bench(pair.side_a, pair.side_b, cfg, wd, blob_size, out_send_reply);
                pair.shutdown();
            }
        }

        void run_stress_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& out_send,
            stress_stats& out_recv)
        {
            tcp_stream_pair pair;
            if (make_tcp_stream_pair(pair))
                run_paired_stream_stress_bench(pair.side_a, pair.side_b, cfg, wd, blob_size, out_send, out_recv);
            pair.shutdown();
        }
    } // namespace

    void add_tcp_blocking_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "tcp_blocking"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tcp_blocking",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tcp(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tcp_blocking",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tcp(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
