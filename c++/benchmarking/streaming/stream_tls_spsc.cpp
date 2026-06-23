/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"
#include "benchmark_tls_fixture.h"

#include <streaming/spsc_queue/stream.h>
#include <streaming/secure_stream.h>

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

        bool make_tls_pair(
            const tls_fixture_cert_pair& cert,
            const std::shared_ptr<streaming::stream>& raw_a,
            const std::shared_ptr<streaming::stream>& raw_b,
            const std::shared_ptr<coro::scheduler>& scheduler_a,
            const std::shared_ptr<coro::scheduler>& scheduler_b,
            std::shared_ptr<streaming::stream>& tls_a,
            std::shared_ptr<streaming::stream>& tls_b)
        {
            auto server_context = std::make_shared<streaming::secure::context>(cert.cert_path, cert.key_path);
            auto client_context = std::make_shared<streaming::secure::client_context>(false);
            if (!server_context->is_valid() || !client_context->is_valid())
                return false;

            coro::sync_wait(
                coro::when_all(
                    scheduler_a->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(raw_a, server_context, scheduler_a);
                            if (co_await stream->handshake())
                                tls_a = stream;
                        }()),
                    scheduler_b->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(raw_b, client_context, scheduler_b);
                            if (co_await stream->client_handshake())
                                tls_b = stream;
                        }())));

            return tls_a && tls_b;
        }

        void run_standard_tls_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (make_tls_pair(cert, pipe->side_a(sched_a), pipe->side_b(sched_b), sched_a, sched_b, tls_a, tls_b))
                run_paired_stream_bench(tls_a, tls_b, cfg, wd, blob_size, unidirectional, send_reply);

            sched_a->shutdown();
            sched_b->shutdown();
        }

        void run_stress_tls_spsc(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            auto sched_a = make_scheduler();
            auto sched_b = make_scheduler();
            auto pipe = std::make_unique<spsc_pipe>();
            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (make_tls_pair(cert, pipe->side_a(sched_a), pipe->side_b(sched_b), sched_a, sched_b, tls_a, tls_b))
                run_paired_stream_stress_bench(tls_a, tls_b, cfg, wd, blob_size, send, recv);

            sched_a->shutdown();
            sched_b->shutdown();
        }
    } // namespace

    void add_tls_spsc_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "tls+spsc"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tls+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tls_spsc(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tls+spsc",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tls_spsc(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
