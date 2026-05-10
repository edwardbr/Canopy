/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <io_uring/host_io_uring.h>
#include <io_uring/tcp.h>
#include <streaming/io_uring/acceptor.h>
#include <streaming/io_uring/stream.h>

namespace stream_bench
{
    namespace
    {
        rpc::io_uring::linux_io_uring_handle::options benchmark_io_uring_options()
        {
            rpc::io_uring::linux_io_uring_handle::options options;
            options.queue_depth = 512;
            options.use_sqpoll = true;
            options.buffer_count = 128;
            options.buffer_size = 65536;
            options.register_buffers = false;
            options.fixed_file_count = 128;
            options.register_fixed_files = true;
            return options;
        }

        std::shared_ptr<rpc::io_uring::io_uring_scheduler> make_benchmark_io_uring_scheduler(
            const std::shared_ptr<coro::scheduler>& scheduler)
        {
            std::shared_ptr<rpc::io_uring::io_uring_scheduler> owner;
            auto controller_options = rpc::io_uring::default_host_controller_options();
            controller_options.completion_wait_strategy = rpc::io_uring::wait_strategy::proactor;
            if (rpc::io_uring::create_host_io_uring_scheduler(
                    owner, benchmark_io_uring_options(), scheduler, controller_options)
                != rpc::error::OK())
            {
                return {};
            }

            return owner;
        }

        void run_io_uring_pair(
            const std::shared_ptr<coro::scheduler>& server_scheduler,
            const std::shared_ptr<rpc::io_uring::controller>& server_controller,
            const std::shared_ptr<rpc::io_uring::controller>& client_controller,
            uint16_t port,
            auto server_fn,
            auto client_fn)
        {
            auto acceptor = std::make_shared<streaming::io_uring::acceptor>(server_controller);
            if (coro::sync_wait(acceptor->listen_loopback(port)) != rpc::error::OK())
                return;
            if (!acceptor->init(server_scheduler))
                return;

            rpc::event server_ready;
            coro::sync_wait(
                coro::when_all(
                    [&]() -> coro::task<void>
                    {
                        server_ready.set();
                        auto maybe_stream = co_await acceptor->accept();
                        if (!maybe_stream)
                            co_return;
                        co_await server_fn(*maybe_stream);
                        acceptor->stop();
                    }(),
                    [&]() -> coro::task<void>
                    {
                        co_await server_ready.wait();
                        rpc::io_uring::connector connector(client_controller);
                        auto connect_result = co_await connector.connect_loopback_with_result(port);
                        auto stream_result = streaming::io_uring::make_stream_result(connect_result, port);
                        if (stream_result.error_code != rpc::error::OK() || !stream_result.connection)
                            co_return;
                        co_await client_fn(std::move(stream_result.connection));
                    }()));
        }

        void run_standard_io_uring(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& out_unidirectional,
            bench_stats& out_send_reply)
        {
            const std::vector<uint8_t> payload(blob_size, 0xab);
            auto server_scheduler = make_scheduler();
            auto client_scheduler = make_scheduler();
            auto server_owner = make_benchmark_io_uring_scheduler(server_scheduler);
            auto client_owner = make_benchmark_io_uring_scheduler(client_scheduler);
            if (!server_owner || !client_owner)
                return;

            auto server_controller = server_owner->get_controller();
            auto client_controller = client_owner->get_controller();
            if (!server_controller || !client_controller)
                return;

            if (cfg.run_unidirectional)
            {
                std::atomic<bool> stop{false};
                run_io_uring_pair(
                    server_scheduler,
                    server_controller,
                    client_controller,
                    allocate_loopback_port(),
                    [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                    { co_await run_drain(stream, stop, wd); },
                    [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                    { out_unidirectional = co_await run_unidirectional_sender(stream, payload, stop, cfg, wd); });
            }

            if (cfg.run_send_reply)
            {
                std::atomic<bool> stop{false};
                run_io_uring_pair(
                    server_scheduler,
                    server_controller,
                    client_controller,
                    allocate_loopback_port(),
                    [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                    { co_await run_echo(stream, stop, wd); },
                    [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                    { out_send_reply = co_await run_send_reply(stream, payload, stop, cfg, wd); });
            }

            server_owner->shutdown();
            client_owner->shutdown();
        }

        void run_stress_io_uring(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& out_send,
            stress_stats& out_recv)
        {
            const std::vector<uint8_t> payload(blob_size, 0xab);
            auto server_scheduler = make_scheduler();
            auto client_scheduler = make_scheduler();
            auto server_owner = make_benchmark_io_uring_scheduler(server_scheduler);
            auto client_owner = make_benchmark_io_uring_scheduler(client_scheduler);
            if (!server_owner || !client_owner)
                return;

            auto server_controller = server_owner->get_controller();
            auto client_controller = client_owner->get_controller();
            if (!server_controller || !client_controller)
                return;

            std::atomic<bool> stop{false};
            run_io_uring_pair(
                server_scheduler,
                server_controller,
                client_controller,
                allocate_loopback_port(),
                [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                { out_recv = co_await run_stress_drain(stream, stop, cfg, wd); },
                [&](std::shared_ptr<streaming::stream> stream) -> coro::task<void>
                { out_send = co_await run_stress_sender(std::move(stream), payload, stop, cfg, wd); });

            server_owner->shutdown();
            client_owner->shutdown();
        }
    } // namespace

    void add_io_uring_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "io_uring"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"io_uring",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_io_uring(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"io_uring",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_io_uring(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
