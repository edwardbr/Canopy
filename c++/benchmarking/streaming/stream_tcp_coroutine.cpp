/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include <io_uring/host_io_uring.h>
#include <io_uring/tcp.h>
#include <streaming/tcp_coroutine/acceptor.h>
#include <streaming/tcp_coroutine/stream.h>

namespace stream_bench
{
    namespace
    {
        rpc::io_uring::linux_io_uring_handle::options benchmark_tcp_coroutine_io_options()
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

        std::shared_ptr<rpc::io_uring::io_uring_scheduler> make_benchmark_tcp_coroutine_scheduler(
            const std::shared_ptr<coro::scheduler>& scheduler)
        {
            std::shared_ptr<rpc::io_uring::io_uring_scheduler> owner;
            auto controller_options = rpc::io_uring::default_controller_options();
            controller_options.completion_wait_strategy = rpc::io_uring::wait_strategy::proactor;
            if (rpc::io_uring::create_scheduler(owner, benchmark_tcp_coroutine_io_options(), scheduler, controller_options)
                != rpc::error::OK())
            {
                return {};
            }

            return owner;
        }

        // NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters):
        // run_tcp_coroutine_pair joins these tasks before server_ready leaves scope.
        template<typename ServerFn>
        coro::task<void> run_tcp_coroutine_server_side(
            std::shared_ptr<streaming::coroutine::tcp::acceptor> acceptor,
            rpc::event& server_ready,
            ServerFn server_fn)
        {
            server_ready.set();
            auto maybe_stream = co_await acceptor->accept();
            if (!maybe_stream)
                co_return;
            co_await server_fn(*maybe_stream);
            acceptor->stop();
        }

        template<typename ClientFn>
        coro::task<void> run_tcp_coroutine_client_side(
            rpc::event& server_ready,
            std::shared_ptr<rpc::io_uring::controller> client_controller,
            uint16_t port,
            std::shared_ptr<coro::scheduler> client_scheduler,
            ClientFn client_fn)
        {
            co_await server_ready.wait();
            rpc::io_uring::connector connector(std::move(client_controller));
            auto connect_result = co_await connector.connect_loopback_with_result(port);
            auto stream_result = streaming::coroutine::tcp::make_stream_result(
                connect_result, port, streaming::coroutine::tcp::default_stream_options(), std::move(client_scheduler));
            if (stream_result.error_code != rpc::error::OK() || !stream_result.connection)
                co_return;
            co_await client_fn(std::move(stream_result.connection));
        }
        // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)

        struct drain_stream_action
        {
            std::atomic<bool>& stop;
            watchdog& wd;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                CO_AWAIT run_drain(std::move(stream), stop, wd);
                CO_RETURN;
            }
        };

        struct echo_stream_action
        {
            std::atomic<bool>& stop;
            watchdog& wd;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                CO_AWAIT run_echo(std::move(stream), stop, wd);
                CO_RETURN;
            }
        };

        struct unidirectional_sender_action
        {
            const std::vector<uint8_t>& payload;
            std::atomic<bool>& stop;
            const bench_config& cfg;
            watchdog& wd;
            bench_stats& output;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                output = CO_AWAIT run_unidirectional_sender(std::move(stream), payload, stop, cfg, wd);
                CO_RETURN;
            }
        };

        struct send_reply_action
        {
            const std::vector<uint8_t>& payload;
            std::atomic<bool>& stop;
            const bench_config& cfg;
            watchdog& wd;
            bench_stats& output;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                output = CO_AWAIT run_send_reply(std::move(stream), payload, stop, cfg, wd);
                CO_RETURN;
            }
        };

        struct stress_drain_action
        {
            std::atomic<bool>& stop;
            const bench_config& cfg;
            watchdog& wd;
            stress_stats& output;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                output = CO_AWAIT run_stress_drain(std::move(stream), stop, cfg, wd);
                CO_RETURN;
            }
        };

        struct stress_sender_action
        {
            const std::vector<uint8_t>& payload;
            std::atomic<bool>& stop;
            const bench_config& cfg;
            watchdog& wd;
            stress_stats& output;

            CORO_TASK(void)
            operator()(std::shared_ptr<streaming::stream> stream) const
            {
                output = CO_AWAIT run_stress_sender(std::move(stream), payload, stop, cfg, wd);
                CO_RETURN;
            }
        };

        void run_tcp_coroutine_pair(
            const std::shared_ptr<coro::scheduler>& server_scheduler,
            const std::shared_ptr<coro::scheduler>& client_scheduler,
            const std::shared_ptr<rpc::io_uring::controller>& server_controller,
            const std::shared_ptr<rpc::io_uring::controller>& client_controller,
            uint16_t port,
            auto server_fn,
            auto client_fn)
        {
            auto acceptor = std::make_shared<streaming::coroutine::tcp::acceptor>(server_controller);
            if (coro::sync_wait(acceptor->listen_loopback(port)) != rpc::error::OK())
                return;
            if (!acceptor->init(server_scheduler))
                return;

            rpc::event server_ready;
            coro::sync_wait(
                coro::when_all(
                    run_tcp_coroutine_server_side(acceptor, server_ready, server_fn),
                    run_tcp_coroutine_client_side(server_ready, client_controller, port, client_scheduler, client_fn)));
        }

        void run_standard_tcp_coroutine(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& out_unidirectional,
            bench_stats& out_send_reply)
        {
            std::vector<uint8_t> payload(blob_size, 0xab);
            auto server_scheduler = make_scheduler();
            auto client_scheduler = make_scheduler();
            auto server_owner = make_benchmark_tcp_coroutine_scheduler(server_scheduler);
            auto client_owner = make_benchmark_tcp_coroutine_scheduler(client_scheduler);
            if (!server_owner || !client_owner)
                return;

            auto server_controller = server_owner->get_controller();
            auto client_controller = client_owner->get_controller();
            if (!server_controller || !client_controller)
                return;

            if (cfg.run_unidirectional)
            {
                std::atomic<bool> stop{false};
                run_tcp_coroutine_pair(
                    server_scheduler,
                    client_scheduler,
                    server_controller,
                    client_controller,
                    allocate_loopback_port(),
                    drain_stream_action{stop, wd},
                    unidirectional_sender_action{payload, stop, cfg, wd, out_unidirectional});
            }

            if (cfg.run_send_reply)
            {
                std::atomic<bool> stop{false};
                run_tcp_coroutine_pair(
                    server_scheduler,
                    client_scheduler,
                    server_controller,
                    client_controller,
                    allocate_loopback_port(),
                    echo_stream_action{stop, wd},
                    send_reply_action{payload, stop, cfg, wd, out_send_reply});
            }

            server_owner->shutdown();
            client_owner->shutdown();
        }

        void run_stress_tcp_coroutine(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            stress_stats& out_send,
            stress_stats& out_recv)
        {
            std::vector<uint8_t> payload(blob_size, 0xab);
            auto server_scheduler = make_scheduler();
            auto client_scheduler = make_scheduler();
            auto server_owner = make_benchmark_tcp_coroutine_scheduler(server_scheduler);
            auto client_owner = make_benchmark_tcp_coroutine_scheduler(client_scheduler);
            if (!server_owner || !client_owner)
                return;

            auto server_controller = server_owner->get_controller();
            auto client_controller = client_owner->get_controller();
            if (!server_controller || !client_controller)
                return;

            std::atomic<bool> stop{false};
            run_tcp_coroutine_pair(
                server_scheduler,
                client_scheduler,
                server_controller,
                client_controller,
                allocate_loopback_port(),
                stress_drain_action{stop, cfg, wd, out_recv},
                stress_sender_action{payload, stop, cfg, wd, out_send});

            server_owner->shutdown();
            client_owner->shutdown();
        }
    } // namespace

    void add_tcp_coroutine_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        if (!should_run_stream(cfg, "tcp_coroutine"))
            return;

        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"tcp_coroutine",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_tcp_coroutine(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }

        if (cfg.run_stress)
        {
            for (const auto blob_size : get_stress_blob_sizes(cfg))
            {
                stress_jobs.push_back(
                    stress_benchmark_job{"tcp_coroutine",
                        blob_size,
                        [&cfg, &wd, blob_size](stress_stats& send, stress_stats& recv)
                        { run_stress_tcp_coroutine(cfg, wd, blob_size, send, recv); }});
            }
        }
    }
}
