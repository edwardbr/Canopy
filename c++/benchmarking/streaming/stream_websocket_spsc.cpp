/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_WEBSOCKET
#  include <streaming/spsc_queue/stream.h>
#  include <streaming/websocket/stream.h>
#endif

namespace stream_bench
{
#ifdef CANOPY_BUILD_WEBSOCKET
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

        void run_standard_websocket_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            auto server = std::make_shared<streaming::websocket::stream>(pipe->side_a(sched_a));
            auto client = std::make_shared<streaming::websocket::stream>(
                pipe->side_b(sched_b), streaming::websocket::stream_role::client);

            run_paired_stream_bench(server, client, cfg, wd, blob_size, unidirectional, send_reply);

            sched_a->shutdown();
            sched_b->shutdown();
        }

        void run_stress_websocket_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            auto server = std::make_shared<streaming::websocket::stream>(pipe->side_a(sched_a));
            auto client = std::make_shared<streaming::websocket::stream>(
                pipe->side_b(sched_b), streaming::websocket::stream_role::client);

            run_paired_stream_stress_bench(server, client, cfg, wd, blob_size, send, recv);

            sched_a->shutdown();
            sched_b->shutdown();
        }
    } // namespace
#endif

    void add_websocket_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
#ifdef CANOPY_BUILD_WEBSOCKET
        if (!should_run_stream(cfg, "ws+spsc"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"ws+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_websocket_spsc(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"ws+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_websocket_spsc(cfg, wd, blob_size, send, recv); }});
            }
        }
#else
        (void)cfg;
        (void)wd;
        (void)standard_jobs;
        (void)stress_jobs;
#endif
    }
}
