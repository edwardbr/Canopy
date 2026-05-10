/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <streaming/tcp/stream.h>

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
            const std::vector<uint8_t> payload(blob_size, 0xab);
            const coro::net::socket_address endpoint{"127.0.0.1", allocate_loopback_port()};
            auto sched_server = make_scheduler();
            auto sched_client = make_scheduler();

            if (cfg.run_unidirectional)
            {
                std::atomic<bool> stop{false};
                rpc::event server_ready;
                coro::sync_wait(
                    coro::when_all(
                        [&]() -> coro::task<void>
                        {
                            auto server = std::make_shared<coro::net::tcp::server>(sched_server, endpoint);
                            server_ready.set();
                            auto accepted = co_await server->accept(std::chrono::milliseconds{5000});
                            if (!accepted)
                                co_return;
                            auto stream = std::make_shared<streaming::tcp::stream>(std::move(*accepted), sched_server);
                            co_await run_drain(stream, stop, wd);
                        }(),
                        [&]() -> coro::task<void>
                        {
                            co_await server_ready.wait();
                            coro::net::tcp::client client(sched_client, endpoint);
                            if (co_await client.connect(std::chrono::milliseconds{5000})
                                != coro::net::connect_status::connected)
                                co_return;
                            auto stream = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                            out_unidirectional = co_await run_unidirectional_sender(stream, payload, stop, cfg, wd);
                        }()));
            }

            if (cfg.run_send_reply)
            {
                std::atomic<bool> stop{false};
                rpc::event server_ready;
                coro::sync_wait(
                    coro::when_all(
                        [&]() -> coro::task<void>
                        {
                            auto server = std::make_shared<coro::net::tcp::server>(sched_server, endpoint);
                            server_ready.set();
                            auto accepted = co_await server->accept(std::chrono::milliseconds{5000});
                            if (!accepted)
                                co_return;
                            auto stream = std::make_shared<streaming::tcp::stream>(std::move(*accepted), sched_server);
                            co_await run_echo(stream, stop, wd);
                        }(),
                        [&]() -> coro::task<void>
                        {
                            co_await server_ready.wait();
                            coro::net::tcp::client client(sched_client, endpoint);
                            if (co_await client.connect(std::chrono::milliseconds{5000})
                                != coro::net::connect_status::connected)
                                co_return;
                            auto stream = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                            out_send_reply = co_await run_send_reply(stream, payload, stop, cfg, wd);
                        }()));
            }

            sched_server->shutdown();
            sched_client->shutdown();
        }

        void run_stress_tcp(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& out_send,
            stress_stats& out_recv)
        {
            const std::vector<uint8_t> payload(blob_size, 0xab);
            const coro::net::socket_address endpoint{"127.0.0.1", allocate_loopback_port()};
            auto sched_server = make_scheduler();
            auto sched_client = make_scheduler();
            std::atomic<bool> stop{false};
            rpc::event server_ready;

            coro::sync_wait(
                coro::when_all(
                    [&]() -> coro::task<void>
                    {
                        auto server = std::make_shared<coro::net::tcp::server>(sched_server, endpoint);
                        server_ready.set();
                        auto accepted = co_await server->accept(std::chrono::milliseconds{5000});
                        if (!accepted)
                            co_return;
                        auto stream = std::make_shared<streaming::tcp::stream>(std::move(*accepted), sched_server);
                        out_recv = co_await run_stress_drain(stream, stop, cfg, wd);
                    }(),
                    [&]() -> coro::task<void>
                    {
                        co_await server_ready.wait();
                        coro::net::tcp::client client(sched_client, endpoint);
                        if (co_await client.connect(std::chrono::milliseconds{5000}) != coro::net::connect_status::connected)
                            co_return;
                        auto stream = std::make_shared<streaming::tcp::stream>(std::move(client), sched_client);
                        out_send = co_await run_stress_sender(stream, payload, stop, cfg, wd);
                    }()));

            sched_server->shutdown();
            sched_client->shutdown();
        }
    } // namespace

    void add_tcp_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "tcp"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tcp",
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
                    stress_benchmark_job{"tcp",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tcp(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
