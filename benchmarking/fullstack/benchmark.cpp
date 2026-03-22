/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Benchmark Demo
 *   Tests transfer performance between two zones across a matrix of:
 *   - Transports (local, libcoro_dynamic_library, ipc direct, ipc dll, spsc, tcp, io_uring) [coroutine build]
 *   - Transports (local, dynamic_library) [non-coroutine build]
 *   - Serialization formats
 *   - Blob sizes
 *
 *   Measures the middle 80% of 1000 RPC calls (drops first/last 10%).
 *
 *   To build and run:
 *   1. cmake --preset Debug_Coroutine -DCANOPY_BUILD_BENCHMARKING=ON
 *   2. cmake --build build --target benchmark
 *   3. ./build/output/benchmark
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <comprehensive/comprehensive_stub.h>

#include <transports/local/transport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <fmt/format.h>

#ifdef CANOPY_BUILD_COROUTINE
#  include <transports/ipc_transport/transport.h>
#  include <transports/libcoro_dynamic_library/transport.h>
#  include <streaming/listener.h>
#  include <streaming/io_uring/acceptor.h>
#  include <streaming/io_uring/stream.h>
#  include <streaming/spsc_queue/stream.h>
#  include <streaming/tcp/acceptor.h>
#  include <streaming/tcp/stream.h>
#  include <transports/streaming/transport.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#else
#  include <transports/dynamic_library/transport.h>
#endif

namespace comprehensive
{
    namespace v1
    {
        using clock_type = std::chrono::steady_clock;

        constexpr bool reduced_debug_benchmark_matrix =
#if defined(NDEBUG)
            false;
#else
            true;
#endif

        constexpr size_t call_count = reduced_debug_benchmark_matrix ? 5 : 1000;
        constexpr size_t trim_each_side = call_count / 10;
        constexpr size_t local_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 10;
        constexpr size_t dll_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 20;
        constexpr size_t ipc_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 30;
        constexpr size_t spsc_warmup_calls = reduced_debug_benchmark_matrix ? 1 : 20;
        constexpr size_t tcp_warmup_calls = reduced_debug_benchmark_matrix ? 2 : 100;
        constexpr size_t io_uring_warmup_calls = reduced_debug_benchmark_matrix ? 2 : 100;

        struct benchmark_stats
        {
            double avg_us = 0.0;
            double min_us = 0.0;
            double max_us = 0.0;
            double p50_us = 0.0;
            double p90_us = 0.0;
            double p95_us = 0.0;
        };

        struct benchmark_result
        {
            int error = rpc::error::OK();
            benchmark_stats stats{};
        };

        struct encoding_info
        {
            rpc::encoding enc;
            const char* name;
        };

        std::vector<uint8_t> make_blob(size_t size)
        {
            std::vector<uint8_t> data(size);
            for (size_t i = 0; i < size; ++i)
            {
                data[i] = static_cast<uint8_t>(i % 251);
            }
            return data;
        }

        benchmark_stats compute_stats(const std::vector<int64_t>& samples)
        {
            benchmark_stats stats{};
            if (samples.size() < (trim_each_side * 2))
            {
                return stats;
            }

            const size_t begin = trim_each_side;
            const size_t end = samples.size() - trim_each_side;
            std::vector<int64_t> mid(samples.begin() + static_cast<long>(begin), samples.begin() + static_cast<long>(end));

            std::sort(mid.begin(), mid.end());
            const size_t mid_count = mid.size();
            if (mid_count == 0)
            {
                return stats;
            }

            const auto sum = std::accumulate(mid.begin(), mid.end(), int64_t{0});
            stats.avg_us = static_cast<double>(sum) / static_cast<double>(mid_count);
            stats.min_us = static_cast<double>(mid.front());
            stats.max_us = static_cast<double>(mid.back());
            stats.p50_us = static_cast<double>(mid[(mid_count * 50) / 100]);
            stats.p90_us = static_cast<double>(mid[(mid_count * 90) / 100]);
            stats.p95_us = static_cast<double>(mid[(mid_count * 95) / 100]);
            return stats;
        }

        void print_stats(const char* transport, const char* encoding, size_t blob_size, const benchmark_stats& stats)
        {
            const double size_mb = static_cast<double>(blob_size) / (1024.0 * 1024.0);

            // Only calculate throughput if avg time is meaningful (>= 0.5 microseconds)
            // Below this threshold, timing precision is insufficient for accurate throughput
            constexpr double min_time_us = 0.5;
            double payload_mb_per_sec = 0.0;
            double round_trip_mb_per_sec = 0.0;
            bool throughput_valid = (stats.avg_us >= min_time_us);

            if (throughput_valid)
            {
                payload_mb_per_sec = size_mb / (stats.avg_us / 1e6);
                round_trip_mb_per_sec = (size_mb * 2.0) / (stats.avg_us / 1e6);
            }

            if (throughput_valid)
            {
                fmt::print(
                    "{:>10} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
                    "{:>8.2f} | max {:>8.2f} | "
                    "payload {:>7.2f} MB/s | round-trip {:>7.2f} MB/s\n",
                    transport,
                    encoding,
                    blob_size,
                    stats.avg_us,
                    stats.p50_us,
                    stats.p90_us,
                    stats.p95_us,
                    stats.min_us,
                    stats.max_us,
                    payload_mb_per_sec,
                    round_trip_mb_per_sec);
            }
            else
            {
                fmt::print(
                    "{:>10} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
                    "{:>8.2f} | max {:>8.2f} | "
                    "payload {:>7} MB/s | round-trip {:>7} MB/s\n",
                    transport,
                    encoding,
                    blob_size,
                    stats.avg_us,
                    stats.p50_us,
                    stats.p90_us,
                    stats.p95_us,
                    stats.min_us,
                    stats.max_us,
                    "N/A",
                    "N/A");
            }
        }

        CORO_TASK(int)
        run_benchmark_calls(rpc::shared_ptr<i_data_processor> remote,
            const std::vector<uint8_t>& payload,
            std::vector<int64_t>& durations_us,
            size_t warmup_count = 0)
        {
            durations_us.clear();
            durations_us.reserve(call_count);

            std::vector<uint8_t> response;

            // Warmup calls to eliminate initialization overhead
            for (size_t i = 0; i < warmup_count; ++i)
            {
                const auto error = CO_AWAIT remote->echo_binary(payload, response);
                if (error != rpc::error::OK())
                {
                    CO_RETURN error;
                }
            }

            // Actual benchmark calls
            for (size_t i = 0; i < call_count; ++i)
            {
                const auto start = clock_type::now();
                const auto error = CO_AWAIT remote->echo_binary(payload, response);
                const auto end = clock_type::now();

                if (error != rpc::error::OK())
                {
                    CO_RETURN error;
                }

                if (response.size() != payload.size())
                {
                    CO_RETURN rpc::error::INVALID_DATA();
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                durations_us.push_back(static_cast<int64_t>(elapsed));
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(benchmark_result)
        run_local_benchmark(
#ifdef CANOPY_BUILD_COROUTINE
            std::shared_ptr<coro::scheduler> scheduler,
#endif
            rpc::encoding enc,
            size_t blob_size)
        {
            benchmark_result result{};

            auto root_service = std::make_shared<rpc::root_service>("benchmark_root",
                rpc::DEFAULT_PREFIX
#ifdef CANOPY_BUILD_COROUTINE
                ,
                scheduler
#endif
            );
            root_service->set_default_encoding(enc);

            auto child_transport = std::make_shared<rpc::local::child_transport>("benchmark_child", root_service);

            child_transport->set_child_entry_point<i_data_processor, i_data_processor>(
                [](const rpc::shared_ptr<i_data_processor>& parent,
                    const std::shared_ptr<rpc::child_service>& child_service_ptr)
                    -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    (void)parent;
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), create_data_processor()};
                });

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                "benchmark_child", child_transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;

            if (error != rpc::error::OK())
            {
                result.error = error;
                CO_RETURN result;
            }

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, local_warmup_calls);
            if (result.error == rpc::error::OK())
            {
                result.stats = compute_stats(durations_us);
            }

            CO_RETURN result;
        }

#ifndef CANOPY_BUILD_COROUTINE
        CORO_TASK(benchmark_result) run_dynamic_library_benchmark(rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};

            auto root_service = std::make_shared<rpc::root_service>("benchmark_dynamic_library", rpc::DEFAULT_PREFIX);
            root_service->set_default_encoding(enc);

            auto child_transport = std::make_shared<rpc::dynamic_library::child_transport>(
                "benchmark_dynamic_library", root_service, CANOPY_BENCHMARK_DLL_PATH);

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                "benchmark_dynamic_library", child_transport, not_used);
            remote_processor = connect_result.output_interface;
            result.error = connect_result.error_code;
            if (result.error != rpc::error::OK())
                CO_RETURN result;

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, dll_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            CO_RETURN result;
        }
#endif

#ifdef CANOPY_BUILD_COROUTINE
        std::shared_ptr<coro::scheduler> make_benchmark_scheduler(uint32_t thread_count = 2)
        {
            return std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = thread_count},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));
        }

        void wait_for_scheduler_cleanup(std::weak_ptr<coro::scheduler> scheduler)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!scheduler.expired() && std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!scheduler.expired())
            {
                std::cerr << "benchmark: scheduler cleanup timed out\n";
            }
        }

        uint16_t allocate_loopback_port()
        {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            RPC_ASSERT(fd >= 0);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;

            const int bind_result = ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
            RPC_ASSERT(bind_result == 0);

            sockaddr_in bound_addr{};
            socklen_t bound_addr_len = sizeof(bound_addr);
            const int getsockname_result = ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_addr_len);
            RPC_ASSERT(getsockname_result == 0);

            ::close(fd);
            return ntohs(bound_addr.sin_port);
        }

        CORO_TASK(benchmark_result)
        run_libcoro_dynamic_library_benchmark(std::shared_ptr<coro::scheduler> scheduler, rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};

            auto root_service = std::make_shared<rpc::root_service>(
                "benchmark_libcoro_dynamic_library", rpc::DEFAULT_PREFIX, scheduler);
            root_service->set_default_encoding(enc);

            auto child_transport = std::make_shared<rpc::libcoro_dynamic_library::child_transport>(
                "benchmark_libcoro_dynamic_library", root_service, CANOPY_BENCHMARK_LIBCORO_DLL_PATH);

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            {
                auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                    "benchmark_libcoro_dynamic_library", child_transport, not_used);
                remote_processor = connect_result.output_interface;
                result.error = connect_result.error_code;
                if (result.error != rpc::error::OK())
                    CO_RETURN result;
            }
            not_used = nullptr;
            child_transport.reset();
            root_service.reset();

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, dll_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            remote_processor = nullptr;

            CO_RETURN result;
        }

        CORO_TASK(benchmark_result)
        run_ipc_direct_benchmark(std::shared_ptr<coro::scheduler> scheduler, rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};
            constexpr size_t ipc_child_scheduler_thread_count = 1;

            auto root_service
                = std::make_shared<rpc::root_service>("benchmark_ipc_direct", rpc::DEFAULT_PREFIX, scheduler);
            root_service->set_default_encoding(enc);

            auto child_zone = rpc::DEFAULT_PREFIX;
            [[maybe_unused]] bool ok = child_zone.set_subnet(child_zone.get_subnet() + 1);
            RPC_ASSERT(ok);

            auto transport = rpc::ipc_transport::make_client("benchmark_ipc_direct",
                root_service,
                rpc::ipc_transport::options{
                    .process_executable = CANOPY_BENCHMARK_IPC_CHILD_PROCESS_PATH,
                    .dll_path = {},
                    .dll_zone = child_zone,
                    .process_kind = rpc::ipc_transport::child_process_kind::direct_service,
                    .child_scheduler_thread_count = ipc_child_scheduler_thread_count,
                    .kill_child_on_parent_death = true,
                });

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            {
                auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                    "benchmark_ipc_direct", transport, not_used);
                remote_processor = connect_result.output_interface;
                result.error = connect_result.error_code;
                if (result.error != rpc::error::OK())
                    CO_RETURN result;
            }
            not_used = nullptr;
            transport.reset();
            root_service.reset();

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, ipc_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            remote_processor = nullptr;

            CO_RETURN result;
        }

        CORO_TASK(benchmark_result)
        run_ipc_dll_benchmark(std::shared_ptr<coro::scheduler> scheduler, rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};
            constexpr size_t ipc_child_scheduler_thread_count = 1;

            auto root_service = std::make_shared<rpc::root_service>("benchmark_ipc_dll", rpc::DEFAULT_PREFIX, scheduler);
            root_service->set_default_encoding(enc);

            auto child_zone = rpc::DEFAULT_PREFIX;
            [[maybe_unused]] bool ok = child_zone.set_subnet(child_zone.get_subnet() + 1);
            RPC_ASSERT(ok);

            auto transport = rpc::ipc_transport::make_client("benchmark_ipc_dll",
                root_service,
                rpc::ipc_transport::options{
                    .process_executable = CANOPY_BENCHMARK_IPC_CHILD_HOST_PROCESS_PATH,
                    .dll_path = CANOPY_BENCHMARK_LIBCORO_SPSC_DLL_PATH,
                    .dll_zone = child_zone,
                    .child_scheduler_thread_count = ipc_child_scheduler_thread_count,
                    .kill_child_on_parent_death = true,
                });

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            {
                auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                    "benchmark_ipc_dll", transport, not_used);
                remote_processor = connect_result.output_interface;
                result.error = connect_result.error_code;
                if (result.error != rpc::error::OK())
                    CO_RETURN result;
            }
            not_used = nullptr;
            transport.reset();
            root_service.reset();

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, ipc_warmup_calls);
            if (result.error == rpc::error::OK())
                result.stats = compute_stats(durations_us);

            remote_processor = nullptr;
            CO_RETURN result;
        }

        struct spsc_queues
        {
            streaming::spsc_queue::queue_type to_process_2;
            streaming::spsc_queue::queue_type to_process_1;
        };

        void pin_spsc_benchmark_queues(const std::shared_ptr<spsc_queues>& queues)
        {
            // The queue storage can still be touched by in-flight SPSC work after the benchmark coroutines return.
            // Pinning only the backing queues avoids the earlier use-after-free without retaining services/transports.
            static auto* pinned_queues = new std::vector<std::shared_ptr<spsc_queues>>();
            pinned_queues->push_back(queues);
        }

        CORO_TASK(void)
        spsc_client_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_1,
            spsc_queues* queues,
            rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            benchmark_result& result)
        {
            CO_AWAIT server_ready.wait();

            auto service = std::make_shared<rpc::root_service>("spsc_client", zone_1, scheduler);
            service->set_default_encoding(enc);

            auto stream_1 = std::make_shared<streaming::spsc_queue::stream>(
                &queues->to_process_1, &queues->to_process_2, scheduler);
            auto transport = rpc::stream_transport::make_client("spsc_transport_1", service, std::move(stream_1));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT service->connect_to_zone<i_data_processor, i_data_processor>(
                "spsc_server", transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;
            not_used = nullptr;
            transport.reset();
            service.reset();

            if (error == rpc::error::OK())
            {
                const auto payload = make_blob(blob_size);
                std::vector<int64_t> durations_us;
                result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, spsc_warmup_calls);
                if (result.error == rpc::error::OK())
                {
                    result.stats = compute_stats(durations_us);
                }
            }
            else
            {
                result.error = error;
            }

            remote_processor.reset();
            client_finished.set();
            CO_RETURN;
        }

        CORO_TASK(void)
        spsc_server_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::zone zone_2,
            spsc_queues* queues,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            benchmark_result& result)
        {
            auto service = std::make_shared<rpc::root_service>("spsc_server", zone_2, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            rpc::event on_connected;

            auto stream_2 = std::make_shared<streaming::spsc_queue::stream>(
                &queues->to_process_2, &queues->to_process_1, scheduler);
            auto transport = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>("spsc_transport_2",
                rpc::stream_transport::transport_factory(std::move(stream_2)),
                // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                [&on_connected](const rpc::shared_ptr<i_data_processor>&,
                    const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    auto local = create_data_processor();
                    on_connected.set();
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local)};
                });

            server_ready.set();
            co_await transport->accept();

            co_await on_connected.wait();
            co_await client_finished.wait();
            transport.reset();
            service.reset();
            co_await shutdown_event->wait();
            CO_RETURN;
        }

        benchmark_result run_spsc_benchmark(rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};
            auto zone_1 = rpc::DEFAULT_PREFIX;
            auto zone_2 = rpc::DEFAULT_PREFIX;
            zone_2.set_subnet(zone_2.get_subnet() + 1);
            auto queues = std::make_shared<spsc_queues>();
            pin_spsc_benchmark_queues(queues);

            auto scheduler_1 = make_benchmark_scheduler(1);
            auto scheduler_2 = make_benchmark_scheduler(1);
            auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
            auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);
            rpc::event server_ready;
            rpc::event client_finished;

            coro::sync_wait(coro::when_all(
                spsc_client_task(scheduler_1, zone_1, queues.get(), server_ready, client_finished, enc, blob_size, result),
                spsc_server_task(scheduler_2, zone_2, queues.get(), server_ready, client_finished, enc, result)));

            scheduler_1.reset();
            scheduler_2.reset();
            wait_for_scheduler_cleanup(weak_scheduler_1);
            wait_for_scheduler_cleanup(weak_scheduler_2);

            return result;
        }

        CORO_TASK(void)
        tcp_server_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            std::atomic<bool>& server_started,
            rpc::encoding enc,
            uint16_t port)
        {
            // std::cerr << "tcp_server: task entered\n";
            auto service = std::make_shared<rpc::root_service>("tcp_server", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            coro::net::tcp::server server(scheduler, coro::net::socket_address{"127.0.0.1", port});
            server_started.store(true, std::memory_order_release);
            // std::cerr << "tcp_server: listening on port " << port << '\n';
            server_ready.set();

            auto accepted = co_await server.accept(std::chrono::milliseconds(5000));
            if (!accepted)
            {
                std::cerr << "tcp_server: accept failed\n";
                CO_RETURN;
            }

            auto tcp_stream = std::make_shared<streaming::tcp::stream>(std::move(*accepted), scheduler);
            auto server_transport = CO_AWAIT service->make_acceptor<i_data_processor, i_data_processor>(
                "server_transport",
                rpc::stream_transport::transport_factory(std::move(tcp_stream)),
                [](const rpc::shared_ptr<i_data_processor>&,
                    const std::shared_ptr<rpc::service>& svc) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                {
                    auto local = create_data_processor();
                    CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local)};
                });

            co_await server_transport->accept();
            co_await client_finished.wait();
            // std::cerr << "tcp_server: client finished\n";
            server_transport.reset();
            service.reset();
        }

        CORO_TASK(void)
        tcp_client_task(std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            std::atomic<bool>& server_started,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            // std::cerr << "tcp_client: task entered\n";
            co_await server_ready.wait();
            // std::cerr << "tcp_client: server_ready signalled\n";

            if (!server_started.load(std::memory_order_acquire))
            {
                std::cerr << "tcp_client: server did not start\n";
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }

            const char* host = "127.0.0.1";

            auto client_service = std::make_shared<rpc::root_service>("tcp_client", rpc::zone_address(2, 1), scheduler);
            client_service->set_default_encoding(enc);

            coro::net::tcp::client client(scheduler, coro::net::socket_address{host, port});

            const auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
            if (connection_status != coro::net::connect_status::connected)
            {
                std::cerr << "tcp_client: connect failed with status " << static_cast<int>(connection_status) << '\n';
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }
            // std::cerr << "tcp_client: connected\n";

            auto tcp_stm = std::make_shared<streaming::tcp::stream>(std::move(client), scheduler);
            auto client_transport
                = rpc::stream_transport::make_client("client_transport", client_service, std::move(tcp_stm));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                "tcp_server", client_transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;
            not_used = nullptr;

            if (error != rpc::error::OK())
            {
                std::cerr << "tcp_client: connect_to_zone failed with error " << error << '\n';
                result.error = error;
                client_finished.set();
                CO_RETURN;
            }
            // std::cerr << "tcp_client: zone connected\n";

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, tcp_warmup_calls);
            // std::cerr << "tcp_client: benchmark calls returned " << result.error << '\n';
            if (result.error == rpc::error::OK())
            {
                result.stats = compute_stats(durations_us);
            }

            remote_processor.reset();
            client_finished.set();
            client_transport.reset();
            client_service.reset();
            // std::cerr << "tcp_client: cleaned up\n";
        }

        benchmark_result run_tcp_benchmark(rpc::encoding enc, size_t blob_size, uint16_t port)
        {
            benchmark_result result{};

            // std::cerr << "run_tcp_benchmark: creating schedulers for port " << port << '\n';
            auto scheduler_1 = make_benchmark_scheduler();
            auto scheduler_2 = make_benchmark_scheduler();
            auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
            auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);

            rpc::event server_ready;
            rpc::event client_finished;
            std::atomic<bool> server_started = false;

            // std::cerr << "run_tcp_benchmark: entering when_all\n";
            coro::sync_wait(coro::when_all(
                tcp_server_task(scheduler_1, server_ready, client_finished, server_started, enc, port),
                tcp_client_task(scheduler_2, server_ready, client_finished, server_started, enc, blob_size, port, result)));
            // std::cerr << "run_tcp_benchmark: when_all returned\n";

            scheduler_1.reset();
            scheduler_2.reset();
            wait_for_scheduler_cleanup(weak_scheduler_1);
            wait_for_scheduler_cleanup(weak_scheduler_2);

            return result;
        }

        CORO_TASK(void)
        io_uring_server_task(std::shared_ptr<coro::scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_connected,
            rpc::encoding enc,
            uint16_t port)
        {
            auto service = std::make_shared<rpc::root_service>("io_uring_server", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);

            canopy::network_config::ip_address addr{};
            addr[0] = 127;
            addr[1] = 0;
            addr[2] = 0;
            addr[3] = 1;

            auto io_uring_listener = std::make_shared<streaming::listener>("io_uring_server_transport",
                std::make_shared<streaming::io_uring::acceptor>(addr, port),
                rpc::stream_transport::make_connection_callback<i_data_processor, i_data_processor>(
                    [](const rpc::shared_ptr<i_data_processor>&, const std::shared_ptr<rpc::service>& service_ptr)
                        -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
                    {
                        auto local_service = create_data_processor();
                        CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), std::move(local_service)};
                    }));
            io_uring_listener->start_listening(service);
            server_ready.set();

            co_await client_connected.wait();
            CO_AWAIT io_uring_listener->stop_listening();
            io_uring_listener.reset();
            service.reset();
        }

        CORO_TASK(void)
        io_uring_client_task(std::shared_ptr<coro::scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_connected,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            co_await server_ready.wait();

            auto client_service
                = std::make_shared<rpc::root_service>("io_uring_client", rpc::zone_address(2, 1), scheduler);
            client_service->set_default_encoding(enc);

            coro::net::tcp::client client(scheduler, coro::net::socket_address{"127.0.0.1", port});

            const auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
            if (connection_status != coro::net::connect_status::connected)
            {
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_connected.set();
                CO_RETURN;
            }

            auto stm = std::make_shared<streaming::io_uring::stream>(std::move(client), scheduler);
            auto client_transport
                = rpc::stream_transport::make_client("io_uring_client_transport", client_service, std::move(stm));

            rpc::shared_ptr<i_data_processor> remote_processor;
            rpc::shared_ptr<i_data_processor> not_used;

            const auto connect_result = CO_AWAIT client_service->connect_to_zone<i_data_processor, i_data_processor>(
                "io_uring_server", client_transport, not_used);
            remote_processor = connect_result.output_interface;
            const auto error = connect_result.error_code;
            not_used = nullptr;

            if (error != rpc::error::OK())
            {
                result.error = error;
                client_connected.set();
                CO_RETURN;
            }

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_us, io_uring_warmup_calls);
            if (result.error == rpc::error::OK())
            {
                result.stats = compute_stats(durations_us);
            }

            remote_processor.reset();
            client_connected.set();
            client_transport.reset();
            client_service.reset();
        }

        benchmark_result run_io_uring_benchmark(rpc::encoding enc, size_t blob_size, uint16_t port)
        {
            benchmark_result result{};

            auto scheduler_1 = make_benchmark_scheduler();
            auto scheduler_2 = make_benchmark_scheduler();
            auto weak_scheduler_1 = std::weak_ptr<coro::scheduler>(scheduler_1);
            auto weak_scheduler_2 = std::weak_ptr<coro::scheduler>(scheduler_2);

            rpc::event server_ready;
            rpc::event client_connected;

            coro::sync_wait(coro::when_all(io_uring_server_task(scheduler_1, server_ready, client_connected, enc, port),
                io_uring_client_task(scheduler_2, server_ready, client_connected, enc, blob_size, port, result)));

            scheduler_1.reset();
            scheduler_2.reset();
            wait_for_scheduler_cleanup(weak_scheduler_1);
            wait_for_scheduler_cleanup(weak_scheduler_2);

            return result;
        }
#endif

        void print_header()
        {
            fmt::print("Benchmark: 1000 RPC calls per test, middle 80% (drop first/last 10%)\n");
#ifdef CANOPY_BUILD_COROUTINE
            fmt::print("Warmup: local=10 calls, libcoro_dll=20 calls, ipc=30 calls, spsc=20 calls, io_uring=100 calls, "
                       "tcp=100 calls (not included in timing)\n");
#else
            fmt::print("Warmup: local=10 calls, dynamic_library=20 calls (not included in timing)\n");
#endif
            fmt::print("Note: Throughput shown as 'N/A' when avg time < 0.5us (insufficient timing precision)\n");
            fmt::print("Units: MB/s = megabytes per second (1 MB = 1024*1024 bytes)\n");
            fmt::print("-----------------------------------------------------------------------------------------------"
                       "---------------------------------------------\n");
            fmt::print(
                "transport   | serialization       | blob bytes | avg (us)     | p50       | p90       | p95      "
                " | min       | max       | "
                "payload MB/s | round-trip MB/s\n");
            fmt::print("-----------------------------------------------------------------------------------------------"
                       "---------------------------------------------\n");
        }

        void print_footer()
        {
            fmt::print("-----------------------------------------------------------------------------------------------"
                       "---------------------------------------------\n");
        }
    }
}

int main()
{
    std::cout << "RPC++ Comprehensive Demo - Benchmark\n";
    std::cout << "====================================\n\n";

    using namespace comprehensive::v1;

    const std::vector<encoding_info> encodings
        = reduced_debug_benchmark_matrix
              ? std::vector<encoding_info>{{rpc::encoding::protocol_buffers, "protocol_buffers"}}
              : std::vector<encoding_info>{
                    {rpc::encoding::yas_binary, "yas_binary"},
                    {rpc::encoding::yas_compressed_binary, "yas_compressed"},
                    {rpc::encoding::protocol_buffers, "protocol_buffers"},
                };

    const std::vector<size_t> blob_sizes = reduced_debug_benchmark_matrix ? std::vector<size_t>{64}
                                                                          : std::vector<size_t>{
                                                                                64,     // 64 B
                                                                                256,    // 256 B
                                                                                1024,   // 1 KB
                                                                                4096,   // 4 KB
                                                                                16384,  // 16 KB
                                                                                65536,  // 64 KB
                                                                                131072, // 128 KB
                                                                                262144, // 256 KB
                                                                                524288, // 512 KB
                                                                                1048576 // 1 MB
                                                                            };

    print_header();
    if (reduced_debug_benchmark_matrix)
        fmt::print("Debug benchmark mode: reduced matrix and sample counts enabled.\n");

#ifdef CANOPY_BUILD_COROUTINE
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto local_scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 2},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

            auto result = coro::sync_wait(run_local_benchmark(local_scheduler, enc.enc, blob_size));

            local_scheduler->shutdown();
            if (result.error != rpc::error::OK())
            {
                fmt::print("local  | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("local", enc.name, blob_size, result.stats);
        }
    }

    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto local_scheduler = std::shared_ptr<coro::scheduler>(coro::scheduler::make_unique(
                coro::scheduler::options{.thread_strategy = coro::scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 2},
                    .execution_strategy = coro::scheduler::execution_strategy_t::process_tasks_on_thread_pool}));

            auto result = coro::sync_wait(run_libcoro_dynamic_library_benchmark(local_scheduler, enc.enc, blob_size));

            local_scheduler->shutdown();
            if (result.error != rpc::error::OK())
            {
                fmt::print("libcoro_dll | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("libcoro_dll", enc.name, blob_size, result.stats);
        }
    }

    for (const auto& enc : encodings)
    {
        {
            auto local_scheduler = make_benchmark_scheduler();
            auto weak_scheduler = std::weak_ptr<coro::scheduler>(local_scheduler);

            for (size_t blob_size : blob_sizes)
            {
                auto result = coro::sync_wait(run_ipc_direct_benchmark(local_scheduler, enc.enc, blob_size));
                if (result.error != rpc::error::OK())
                {
                    fmt::print("ipc_direct | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                    continue;
                }
                print_stats("ipc_direct", enc.name, blob_size, result.stats);
            }

            local_scheduler.reset();
            wait_for_scheduler_cleanup(weak_scheduler);
        }
    }

    for (const auto& enc : encodings)
    {
        {
            auto local_scheduler = make_benchmark_scheduler();
            auto weak_scheduler = std::weak_ptr<coro::scheduler>(local_scheduler);

            for (size_t blob_size : blob_sizes)
            {
                auto result = coro::sync_wait(run_ipc_dll_benchmark(local_scheduler, enc.enc, blob_size));
                if (result.error != rpc::error::OK())
                {
                    fmt::print("ipc_dll | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                    continue;
                }
                print_stats("ipc_dll", enc.name, blob_size, result.stats);
            }

            local_scheduler.reset();
            wait_for_scheduler_cleanup(weak_scheduler);
        }
    }

    fmt::print("run_spsc_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_spsc_benchmark(enc.enc, blob_size);
            if (result.error != rpc::error::OK())
            {
                fmt::print("spsc   | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("spsc", enc.name, blob_size, result.stats);
        }
    }

    fmt::print("run_tcp_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_tcp_benchmark(enc.enc, blob_size, allocate_loopback_port());
            if (result.error != rpc::error::OK())
            {
                fmt::print("tcp    | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("tcp", enc.name, blob_size, result.stats);
        }
    }

    fmt::print("run_io_uring_benchmark\n");
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_io_uring_benchmark(enc.enc, blob_size, allocate_loopback_port());
            if (result.error != rpc::error::OK())
            {
                fmt::print("io_uring | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("io_uring", enc.name, blob_size, result.stats);
        }
    }
#else
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_local_benchmark(enc.enc, blob_size);
            if (result.error != rpc::error::OK())
            {
                fmt::print("local  | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("local", enc.name, blob_size, result.stats);
        }
    }

    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_dynamic_library_benchmark(enc.enc, blob_size);
            if (result.error != rpc::error::OK())
            {
                fmt::print("dll        | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("dll", enc.name, blob_size, result.stats);
        }
    }
#endif

    print_footer();
    return 0;
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[TRACE] " << message << std::endl;
        break;
    case 1:
        std::cout << "[DEBUG] " << message << std::endl;
        break;
    case 2:
        std::cout << "[INFO] " << message << std::endl;
        break;
    case 3:
        std::cout << "[WARN] " << message << std::endl;
        break;
    case 4:
        std::cout << "[ERROR] " << message << std::endl;
        break;
    case 5:
        std::cout << "[CRITICAL] " << message << std::endl;
        break;
    default:
        std::cout << "[LOG " << level << "] " << message << std::endl;
        break;
    }
}
