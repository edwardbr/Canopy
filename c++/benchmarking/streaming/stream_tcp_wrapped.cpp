/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"
#include "benchmark_tls_fixture.h"
#include "websocket_client_stream.h"

#ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
#  include <streaming/secure_stream.h>
#endif

#ifdef CANOPY_BUILD_WEBSOCKET
#  include <streaming/websocket/stream.h>
#endif

namespace stream_bench
{
    namespace
    {
        template<class PairBuilder>
        void run_standard_with_fresh_pairs(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply,
            PairBuilder build_pair)
        {
            if (cfg.run_unidirectional)
            {
                tcp_stream_pair raw;
                std::shared_ptr<streaming::stream> side_a;
                std::shared_ptr<streaming::stream> side_b;
                if (build_pair(raw, side_a, side_b))
                    run_stream_unidirectional_bench(side_a, side_b, cfg, wd, blob_size, unidirectional);
                raw.shutdown();
            }

            if (cfg.run_send_reply)
            {
                tcp_stream_pair raw;
                std::shared_ptr<streaming::stream> side_a;
                std::shared_ptr<streaming::stream> side_b;
                if (build_pair(raw, side_a, side_b))
                    run_stream_send_reply_bench(side_a, side_b, cfg, wd, blob_size, send_reply);
                raw.shutdown();
            }
        }

#ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
        bool make_tls_pair(
            tcp_stream_pair& raw,
            const tls_fixture_cert_pair& cert,
            std::shared_ptr<streaming::stream>& tls_a,
            std::shared_ptr<streaming::stream>& tls_b)
        {
            auto server_context = std::make_shared<streaming::secure::context>(cert.cert_path, cert.key_path);
            auto client_context = std::make_shared<streaming::secure::client_context>(false);
            if (!server_context->is_valid() || !client_context->is_valid())
                return false;

#  ifdef CANOPY_BUILD_COROUTINE
            coro::sync_wait(
                coro::when_all(
                    raw.scheduler_a->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(raw.side_a, server_context);
                            if (co_await stream->handshake())
                                tls_a = stream;
                        }()),
                    raw.scheduler_b->schedule(
                        [&]() -> coro::task<void>
                        {
                            auto stream = std::make_shared<streaming::secure::stream>(raw.side_b, client_context);
                            if (co_await stream->client_handshake())
                                tls_b = stream;
                        }())));
#  else
            std::thread server_thread(
                [&]()
                {
                    auto stream = std::make_shared<streaming::secure::stream>(raw.side_a, server_context);
                    if (stream->handshake())
                        tls_a = stream;
                });
            std::thread client_thread(
                [&]()
                {
                    auto stream = std::make_shared<streaming::secure::stream>(raw.side_b, client_context);
                    if (stream->client_handshake())
                        tls_b = stream;
                });
            server_thread.join();
            client_thread.join();
#  endif

            return tls_a && tls_b;
        }

        void run_standard_tls_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            run_standard_with_fresh_pairs(
                cfg,
                wd,
                blob_size,
                unidirectional,
                send_reply,
                [&cert](
                    tcp_stream_pair& raw, std::shared_ptr<streaming::stream>& side_a, std::shared_ptr<streaming::stream>& side_b)
                { return make_tcp_stream_pair(raw) && make_tls_pair(raw, cert, side_a, side_b); });
        }

        void run_stress_tls_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            tcp_stream_pair raw;
            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (make_tcp_stream_pair(raw) && make_tls_pair(raw, cert, tls_a, tls_b))
                run_paired_stream_stress_bench(tls_a, tls_b, cfg, wd, blob_size, send, recv);
            raw.shutdown();
        }
#endif

#ifdef CANOPY_BUILD_WEBSOCKET
        bool make_websocket_tcp_pair(
            tcp_stream_pair& raw,
            std::shared_ptr<streaming::stream>& server,
            std::shared_ptr<streaming::stream>& client)
        {
            if (!make_tcp_stream_pair(raw))
                return false;

            server = std::make_shared<streaming::websocket::stream>(raw.side_a);
            client = std::make_shared<websocket_client_stream>(raw.side_b);
            return true;
        }

        void run_standard_websocket_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            run_standard_with_fresh_pairs(
                cfg,
                wd,
                blob_size,
                unidirectional,
                send_reply,
                [](tcp_stream_pair& raw, std::shared_ptr<streaming::stream>& side_a, std::shared_ptr<streaming::stream>& side_b)
                { return make_websocket_tcp_pair(raw, side_a, side_b); });
        }

        void run_stress_websocket_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            tcp_stream_pair raw;
            std::shared_ptr<streaming::stream> server;
            std::shared_ptr<streaming::stream> client;
            if (make_websocket_tcp_pair(raw, server, client))
                run_paired_stream_stress_bench(server, client, cfg, wd, blob_size, send, recv);
            raw.shutdown();
        }
#endif

#if defined(CANOPY_STREAMING_BENCHMARK_HAS_TLS) && defined(CANOPY_BUILD_WEBSOCKET)
        bool make_tls_websocket_tcp_pair(
            tcp_stream_pair& raw,
            const tls_fixture_cert_pair& cert,
            std::shared_ptr<streaming::stream>& server,
            std::shared_ptr<streaming::stream>& client)
        {
            if (!make_tcp_stream_pair(raw))
                return false;

            std::shared_ptr<streaming::stream> tls_a;
            std::shared_ptr<streaming::stream> tls_b;
            if (!make_tls_pair(raw, cert, tls_a, tls_b))
                return false;

            server = std::make_shared<streaming::websocket::stream>(tls_a);
            client = std::make_shared<websocket_client_stream>(tls_b);
            return true;
        }

        void run_standard_tls_websocket_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& unidirectional,
            bench_stats& send_reply)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            run_standard_with_fresh_pairs(
                cfg,
                wd,
                blob_size,
                unidirectional,
                send_reply,
                [&cert](
                    tcp_stream_pair& raw, std::shared_ptr<streaming::stream>& side_a, std::shared_ptr<streaming::stream>& side_b)
                { return make_tls_websocket_tcp_pair(raw, cert, side_a, side_b); });
        }

        void run_stress_tls_websocket_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& send,
            stress_stats& recv)
        {
            tls_fixture_cert_pair cert;
            if (!cert.valid)
                return;

            tcp_stream_pair raw;
            std::shared_ptr<streaming::stream> server;
            std::shared_ptr<streaming::stream> client;
            if (make_tls_websocket_tcp_pair(raw, cert, server, client))
                run_paired_stream_stress_bench(server, client, cfg, wd, blob_size, send, recv);
            raw.shutdown();
        }
#endif
    } // namespace

    void add_tls_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
#ifdef CANOPY_STREAMING_BENCHMARK_HAS_TLS
        if (!should_run_stream(cfg, "tls+tcp"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tls+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tls_tcp(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tls+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tls_tcp(cfg, wd, blob_size, send, recv); }});
            }
        }
#else
        (void)cfg;
        (void)wd;
        (void)standard_jobs;
        (void)stress_jobs;
#endif
    }

    void add_websocket_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
#ifdef CANOPY_BUILD_WEBSOCKET
        if (!should_run_stream(cfg, "ws+tcp"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"ws+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_websocket_tcp(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"ws+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_websocket_tcp(cfg, wd, blob_size, send, recv); }});
            }
        }
#else
        (void)cfg;
        (void)wd;
        (void)standard_jobs;
        (void)stress_jobs;
#endif
    }

    void add_tls_websocket_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
#if defined(CANOPY_STREAMING_BENCHMARK_HAS_TLS) && defined(CANOPY_BUILD_WEBSOCKET)
        if (!should_run_stream(cfg, "tls+ws+tcp"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tls+ws+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tls_websocket_tcp(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tls+ws+tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tls_websocket_tcp(cfg, wd, blob_size, send, recv); }});
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
