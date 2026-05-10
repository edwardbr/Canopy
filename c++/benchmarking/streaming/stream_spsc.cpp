/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <streaming/spsc_queue/stream.h>

namespace stream_bench
{
    namespace
    {
        struct spsc_pipe
        {
            streaming::spsc_queue::queue_type q_a_to_b;
            streaming::spsc_queue::queue_type q_b_to_a;

            std::shared_ptr<streaming::stream> side_a(std::shared_ptr<coro::scheduler> scheduler)
            {
                return std::make_shared<streaming::spsc_queue::stream>(&q_a_to_b, &q_b_to_a, std::move(scheduler));
            }

            std::shared_ptr<streaming::stream> side_b(std::shared_ptr<coro::scheduler> scheduler)
            {
                return std::make_shared<streaming::spsc_queue::stream>(&q_b_to_a, &q_a_to_b, std::move(scheduler));
            }
        };

        void run_standard_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();

            run_paired_stream_bench(
                pipe->side_a(sched_a), pipe->side_b(sched_b), cfg, wd, blob_size, unidirectional, send_reply);

            sched_a->shutdown();
            sched_b->shutdown();
        }

        void run_stress_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();

            run_paired_stream_stress_bench(pipe->side_a(sched_a), pipe->side_b(sched_b), cfg, wd, blob_size, send, recv);

            sched_a->shutdown();
            sched_b->shutdown();
        }
    } // namespace

    void add_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "spsc"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_spsc(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_spsc(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
