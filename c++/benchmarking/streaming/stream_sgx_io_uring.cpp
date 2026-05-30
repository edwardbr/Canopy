/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#if defined(CANOPY_STREAMING_BENCHMARK_SGX_IO_URING)

#  include "sgx_coroutine_test_host.h"
#  include "test_globals.h"

#  include <io_uring/host_controller.h>
#  include <io_uring_test/test.h>
#  include <transports/sgx_coroutine/host/connect.h>
#  include <transports/sgx_coroutine/host/transport.h>

#  include <limits>
#  include <utility>

#endif

namespace stream_bench
{
#if defined(CANOPY_STREAMING_BENCHMARK_SGX_IO_URING)
    namespace
    {
        rpc::io_uring::host_controller::options make_benchmark_host_controller_options()
        {
            rpc::io_uring::host_controller::options options;
            options.queue_depth = 512;
            options.use_sqpoll = true;
            options.buffer_count = 128;
            options.buffer_size = 65536;
            options.register_buffers = false;
            options.fixed_file_count = 128;
            options.register_fixed_files = true;
            return options;
        }

        bench_stats to_bench_stats(const io_uring_test::stream_benchmark_stats& enclave_stats)
        {
            bench_stats stats;
            stats.avg = enclave_stats.avg;
            stats.min = enclave_stats.min_value;
            stats.max = enclave_stats.max_value;
            stats.p50 = enclave_stats.p50;
            stats.p90 = enclave_stats.p90;
            stats.p95 = enclave_stats.p95;
            stats.blob_size = static_cast<size_t>(enclave_stats.blob_size);
            stats.valid = enclave_stats.valid;
            return stats;
        }

        class enclave_stream_benchmark_session
        {
        public:
            bool set_up()
            {
                scheduler_ = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                    coro::scheduler::options{
                        .thread_strategy = coro::scheduler::thread_strategy_t::manual,
                        .pool = coro::thread_pool::options{.thread_count = 1},
                    }));
                root_service_ = rpc::root_service::create("sgx streaming benchmark host", rpc::DEFAULT_PREFIX, scheduler_);
                current_host_service = root_service_;

                auto host_result = enclave_connection_test_host::create_for_test();
                if (host_result.error_code != rpc::error::OK() || !host_result.output_interface)
                {
                    RPC_ERROR("failed to create SGX streaming benchmark host interface");
                    return false;
                }

                host_ = std::move(host_result.output_interface);

                auto transport = std::make_shared<rpc::sgx_coroutine_transport::host::transport>(
                    "sgx streaming benchmark enclave", root_service_, sgx_coroutine_test_enclave_path);
                transports_.push_back(transport);

                auto result = SYNC_WAIT(
                    (rpc::sgx_coroutine_transport::host::connect_to_enclave_zone<yyy::i_host, io_uring_test::i_test_uring>(
                        root_service_,
                        "sgx streaming benchmark enclave",
                        transport,
                        host_,
                        make_benchmark_host_controller_options())));

                if (result.error_code != rpc::error::OK() || !result.output_interface)
                {
                    RPC_ERROR("failed to connect SGX streaming benchmark enclave: {}", result.error_code);
                    return false;
                }

                test_uring_ = std::move(result.output_interface);
                return true;
            }

            void tear_down()
            {
                auto scheduler = scheduler_;
                if (!scheduler)
                {
                    return;
                }

                auto shutdown_event = make_root_shutdown_event();
                release_interfaces_and_root_service(shutdown_event);

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
                while (!all_transports_expired() && std::chrono::steady_clock::now() < deadline)
                {
                    scheduler->process_events(std::chrono::milliseconds{1});
                }

                if (!all_transports_expired())
                {
                    RPC_WARNING("SGX streaming benchmark transport did not expire during teardown");
                }
                transports_.clear();

                scheduler->shutdown();
                scheduler_.reset();
                current_host_service.reset();
            }

            ~enclave_stream_benchmark_session() { tear_down(); }

            rpc::shared_ptr<io_uring_test::i_test_uring> test_uring() const { return test_uring_; }

        private:
            bool all_transports_expired() const
            {
                for (const auto& transport : transports_)
                {
                    if (!transport.expired())
                    {
                        return false;
                    }
                }
                return true;
            }

            std::shared_ptr<rpc::event> make_root_shutdown_event()
            {
                auto shutdown_event = std::make_shared<rpc::event>(false);
                if (root_service_)
                {
                    root_service_->set_shutdown_event(shutdown_event);
                }
                return shutdown_event;
            }

            CORO_TASK(void) release_interfaces_for_teardown()
            {
                test_uring_ = nullptr;
                host_ = nullptr;
                interfaces_released_.store(true, std::memory_order_release);
                CO_RETURN;
            }

            CORO_TASK(void) reset_root_service_for_teardown(std::shared_ptr<rpc::event> shutdown_event)
            {
                root_service_.reset();
                current_host_service.reset();
                if (shutdown_event)
                {
                    CO_AWAIT shutdown_event->wait();
                }
                root_shutdown_complete_.store(true, std::memory_order_release);
                CO_RETURN;
            }

            void release_interfaces_and_root_service(const std::shared_ptr<rpc::event>& shutdown_event)
            {
                interfaces_released_.store(false, std::memory_order_release);
                if (!scheduler_->spawn_detached(release_interfaces_for_teardown()))
                {
                    interfaces_released_.store(true, std::memory_order_release);
                }
                while (!interfaces_released_.load(std::memory_order_acquire))
                {
                    scheduler_->process_events(std::chrono::milliseconds{1});
                }

                root_shutdown_complete_.store(false, std::memory_order_release);
                if (!scheduler_->spawn_detached(reset_root_service_for_teardown(shutdown_event)))
                {
                    root_shutdown_complete_.store(true, std::memory_order_release);
                }
                while (!root_shutdown_complete_.load(std::memory_order_acquire))
                {
                    scheduler_->process_events(std::chrono::milliseconds{1});
                }
            }

            std::shared_ptr<rpc::root_service> root_service_;
            std::shared_ptr<coro::scheduler> scheduler_;
            rpc::shared_ptr<yyy::i_host> host_;
            rpc::shared_ptr<io_uring_test::i_test_uring> test_uring_;
            std::vector<std::weak_ptr<rpc::sgx_coroutine_transport::host::transport>> transports_;
            std::atomic_bool interfaces_released_{false};
            std::atomic_bool root_shutdown_complete_{false};
        };

        bool fits_enclave_benchmark_call(
            const bench_config& cfg,
            size_t blob_size)
        {
            return cfg.count <= std::numeric_limits<uint32_t>::max() && cfg.warmup <= std::numeric_limits<uint32_t>::max()
                   && blob_size <= std::numeric_limits<uint32_t>::max();
        }

        void run_standard_sgx_io_uring(
            const bench_config& cfg,
            watchdog& wd,
            size_t blob_size,
            bench_stats& out_unidirectional,
            bench_stats& out_send_reply)
        {
            if (!fits_enclave_benchmark_call(cfg, blob_size))
            {
                return;
            }

            enclave_stream_benchmark_session session;
            if (!session.set_up() || !session.test_uring())
            {
                return;
            }

            const auto iterations = static_cast<uint32_t>(cfg.count);
            const auto warmup = static_cast<uint32_t>(cfg.warmup);
            const auto payload_size = static_cast<uint32_t>(blob_size);

            if (cfg.run_unidirectional)
            {
                wd.heartbeat();
                io_uring_test::stream_benchmark_stats enclave_stats;
                auto err = SYNC_WAIT(
                    session.test_uring()->stream_benchmark(false, iterations, warmup, payload_size, enclave_stats));
                wd.heartbeat();
                if (err == rpc::error::OK())
                {
                    out_unidirectional = to_bench_stats(enclave_stats);
                }
                else
                {
                    RPC_WARNING("SGX io_uring unidirectional benchmark failed: {}", err);
                }
            }

            if (cfg.run_send_reply)
            {
                wd.heartbeat();
                io_uring_test::stream_benchmark_stats enclave_stats;
                auto err = SYNC_WAIT(
                    session.test_uring()->stream_benchmark(true, iterations, warmup, payload_size, enclave_stats));
                wd.heartbeat();
                if (err == rpc::error::OK())
                {
                    out_send_reply = to_bench_stats(enclave_stats);
                }
                else
                {
                    RPC_WARNING("SGX io_uring send-reply benchmark failed: {}", err);
                }
            }
        }
    } // namespace
#endif

    void add_sgx_io_uring_jobs(
        const bench_config& cfg,
        watchdog& wd,
        std::vector<standard_benchmark_job>& standard_jobs,
        std::vector<stress_benchmark_job>& stress_jobs)
    {
        (void)stress_jobs;
        if (!should_run_stream(cfg, "sgx_io_uring"))
        {
            return;
        }

#if defined(CANOPY_STREAMING_BENCHMARK_SGX_IO_URING)
        if (cfg.run_unidirectional || cfg.run_send_reply)
        {
            for (const auto blob_size : get_blob_sizes(cfg))
            {
                standard_jobs.push_back(
                    standard_benchmark_job{"sgx_io_uring",
                        blob_size,
                        [&cfg, &wd, blob_size](bench_stats& unidirectional, bench_stats& send_reply)
                        { run_standard_sgx_io_uring(cfg, wd, blob_size, unidirectional, send_reply); }});
            }
        }
#else
        (void)cfg;
        (void)wd;
        (void)standard_jobs;
#endif
    }
} // namespace stream_bench
