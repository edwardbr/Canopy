/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/*
 *   Benchmark Demo
 *   Tests transfer performance between two zones across a matrix of:
 *   - Transports (local, spsc, tcp)
 *   - Serialization formats
 *   - Blob sizes
 *
 *   Measures the middle 80% of 1000 RPC calls (drops first/last 10%).
 *
 *   To build and run:
 *   1. cmake --preset Coroutine_Debug
 *   2. cmake --build build --target benchmark
 *   3. ./build/output/debug/demos/comprehensive/benchmark
 */

#include <demo_impl.h>
#include <rpc/rpc.h>
#include <comprehensive/comprehensive_stub.h>

#include <transports/local/transport.h>
#include <fmt/core.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifdef CANOPY_BUILD_COROUTINE
#include <transports/spsc/transport.h>
#include <transports/tcp/listener.h>
#include <transports/tcp/transport.h>
#endif

namespace comprehensive
{
    namespace v1
    {
        using clock_type = std::chrono::steady_clock;

        constexpr size_t call_count = 1000;
        constexpr size_t trim_each_side = call_count / 10;

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
                    "{:>6} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
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
                    "{:>6} | {:>18} | {:>9} | avg {:>8.2f} us | p50 {:>8.2f} | p90 {:>8.2f} | p95 {:>8.2f} | min "
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

#ifdef CANOPY_BUILD_COROUTINE
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
        run_local_benchmark(std::shared_ptr<coro::io_scheduler> scheduler, rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};

            std::atomic<uint64_t> zone_gen{0};
            auto root_service = std::make_shared<rpc::service>("benchmark_root", rpc::zone{++zone_gen}, scheduler);
            root_service->set_default_encoding(enc);
            comprehensive_idl_register_stubs(root_service);

            rpc::zone child_zone_id{++zone_gen};
            auto child_transport
                = std::make_shared<rpc::local::child_transport>("benchmark_child", root_service, child_zone_id);

            child_transport->set_child_entry_point<i_data_processor, i_data_processor>(
                [enc](const rpc::shared_ptr<i_data_processor>& parent,
                    rpc::shared_ptr<i_data_processor>& new_service,
                    const std::shared_ptr<rpc::child_service>& child_service_ptr) -> CORO_TASK(int)
                {
                    (void)parent;
                    child_service_ptr->set_default_encoding(enc);
                    new_service = create_data_processor();
                    CO_RETURN rpc::error::OK();
                });

            rpc::shared_ptr<i_data_processor> remote_service;
            rpc::shared_ptr<i_data_processor> input_service; // = create_data_processor();

            const auto error = CO_AWAIT root_service->connect_to_zone(
                "benchmark_child", child_transport, input_service, remote_service);

            if (error != rpc::error::OK())
            {
                result.error = error;
                CO_RETURN result;
            }

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            // Local transport needs minimal warmup
            constexpr size_t local_warmup_calls = 10;
            result.error = CO_AWAIT run_benchmark_calls(remote_service, payload, durations_us, local_warmup_calls);
            if (result.error == rpc::error::OK())
            {
                result.stats = compute_stats(durations_us);
            }

            CO_RETURN result;
        }

        struct spsc_queues
        {
            rpc::spsc::queue_type to_process_2;
            rpc::spsc::queue_type to_process_1;
        };

        CORO_TASK(void)
        spsc_process_1_task(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::zone zone_1,
            rpc::zone zone_2,
            spsc_queues* queues,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            benchmark_result& result)
        {
            auto service_1 = std::make_shared<rpc::service>("spsc_client", zone_1, scheduler);
            service_1->set_default_encoding(enc);
            comprehensive_idl_register_stubs(service_1);

            auto on_shutdown_event = std::make_shared<rpc::event>();
            service_1->set_shutdown_event(on_shutdown_event);

            auto transport_1 = rpc::spsc::spsc_transport::create(
                "spsc_transport_1", service_1, zone_2, &queues->to_process_1, &queues->to_process_2, nullptr);

            rpc::shared_ptr<i_data_processor> remote_service;
            rpc::shared_ptr<i_data_processor> input_service;

            const auto error
                = CO_AWAIT service_1->connect_to_zone("spsc_server", transport_1, input_service, remote_service);

            service_1.reset();
            transport_1.reset();

            if (error == rpc::error::OK())
            {
                const auto payload = make_blob(blob_size);
                std::vector<int64_t> durations_us;
                // SPSC transport needs modest warmup
                constexpr size_t spsc_warmup_calls = 20;
                result.error = CO_AWAIT run_benchmark_calls(remote_service, payload, durations_us, spsc_warmup_calls);
                if (result.error == rpc::error::OK())
                {
                    result.stats = compute_stats(durations_us);
                }
            }
            else
            {
                result.error = error;
            }

            remote_service.reset();
            client_finished.set();
            co_await on_shutdown_event->wait();
        }

        CORO_TASK(void)
        spsc_process_2_task(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::zone zone_2,
            rpc::zone zone_1,
            spsc_queues* queues,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc)
        {
            auto on_shutdown_event = std::make_shared<rpc::event>();
            auto service_2 = std::make_shared<rpc::service>("spsc_server", zone_2, scheduler);
            service_2->set_shutdown_event(on_shutdown_event);
            service_2->set_default_encoding(enc);
            comprehensive_idl_register_stubs(service_2);

            rpc::event on_connected;
            auto handler = [&, enc](const rpc::interface_descriptor& input_interface,
                               rpc::interface_descriptor& output_interface,
                               std::shared_ptr<rpc::service> service,
                               std::shared_ptr<rpc::spsc::spsc_transport> transport) -> CORO_TASK(int)
            {
                auto ret = CO_AWAIT service->attach_remote_zone<i_data_processor, i_data_processor>("spsc_client_proxy",
                    transport,
                    input_interface,
                    output_interface,
                    [&, enc](const rpc::shared_ptr<i_data_processor>& parent,
                        rpc::shared_ptr<i_data_processor>& new_service,
                        const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                    {
                        (void)parent;
                        service_ptr->set_default_encoding(enc);
                        new_service = create_data_processor();
                        on_connected.set();
                        CO_RETURN rpc::error::OK();
                    });
                CO_RETURN ret;
            };

            auto transport_2 = rpc::spsc::spsc_transport::create(
                "spsc_transport_2", service_2, zone_1, &queues->to_process_2, &queues->to_process_1, handler);

            co_await transport_2->accept();
            server_ready.set();

            co_await on_connected.wait();
            service_2.reset();
            transport_2.reset();

            co_await client_finished.wait();
            co_await on_shutdown_event->wait();
        }

        benchmark_result run_spsc_benchmark(rpc::encoding enc, size_t blob_size)
        {
            benchmark_result result{};
            rpc::zone zone_1{1};
            rpc::zone zone_2{2};
            auto queues = std::make_shared<spsc_queues>();

            auto scheduler_1 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            auto scheduler_2 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 1},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            rpc::event server_ready;
            rpc::event client_finished;

            coro::sync_wait(coro::when_all(
                spsc_process_1_task(scheduler_1, zone_1, zone_2, queues.get(), client_finished, enc, blob_size, result),
                spsc_process_2_task(scheduler_2, zone_2, zone_1, queues.get(), server_ready, client_finished, enc)));

            scheduler_1->shutdown();
            scheduler_2->shutdown();

            return result;
        }

        CORO_TASK(void)
        tcp_server_task(std::shared_ptr<coro::io_scheduler> scheduler,
            rpc::event& server_ready,
            const rpc::event& client_finished,
            rpc::encoding enc,
            uint16_t port)
        {
            std::atomic<uint64_t> zone_gen{0};
            auto on_shutdown_event = std::make_shared<rpc::event>();

            auto service = std::make_shared<rpc::service>("tcp_server", rpc::zone{++zone_gen}, scheduler);
            service->set_default_encoding(enc);
            service->set_shutdown_event(on_shutdown_event);
            comprehensive_idl_register_stubs(service);

            auto listener = std::make_shared<rpc::tcp::listener>(
                [enc](const rpc::interface_descriptor& input_descr,
                    rpc::interface_descriptor& output_interface,
                    std::shared_ptr<rpc::service> child_service_ptr,
                    std::shared_ptr<rpc::tcp::tcp_transport> transport) -> CORO_TASK(int)
                {
                    auto ret = CO_AWAIT child_service_ptr->attach_remote_zone<i_data_processor, i_data_processor>(
                        "tcp_client_proxy",
                        transport,
                        input_descr,
                        output_interface,
                        [enc](const rpc::shared_ptr<i_data_processor>& parent,
                            rpc::shared_ptr<i_data_processor>& local_service,
                            const std::shared_ptr<rpc::service>& service_ptr) -> CORO_TASK(int)
                        {
                            (void)parent;
                            service_ptr->set_default_encoding(enc);
                            local_service = create_data_processor();
                            CO_RETURN rpc::error::OK();
                        });
                    CO_RETURN ret;
                },
                std::chrono::milliseconds(100000));

            const auto server_options = coro::net::tcp::server::options{
                .address = coro::net::ip_address::from_string("127.0.0.1"), .port = port, .backlog = 10};

            if (!listener->start_listening(service, server_options))
            {
                server_ready.set();
                CO_RETURN;
            }

            service.reset();
            server_ready.set();

            co_await client_finished.wait();
            co_await listener->stop_listening();
            listener.reset();
            co_await on_shutdown_event->wait();
        }

        CORO_TASK(void)
        tcp_client_task(std::shared_ptr<coro::io_scheduler> scheduler,
            const rpc::event& server_ready,
            rpc::event& client_finished,
            rpc::encoding enc,
            size_t blob_size,
            uint16_t port,
            benchmark_result& result)
        {
            co_await server_ready.wait();

            std::atomic<uint64_t> zone_gen{100};
            const char* host = "127.0.0.1";

            auto peer_zone_id = rpc::zone{1};
            auto client_service = std::make_shared<rpc::service>("tcp_client", rpc::zone{++zone_gen}, scheduler);
            client_service->set_default_encoding(enc);
            comprehensive_idl_register_stubs(client_service);

            coro::net::tcp::client client(scheduler,
                coro::net::tcp::client::options{.address = coro::net::ip_address::from_string(host), .port = port});

            const auto connection_status = CO_AWAIT client.connect(std::chrono::milliseconds(5000));
            if (connection_status != coro::net::connect_status::connected)
            {
                result.error = rpc::error::ZONE_NOT_FOUND();
                client_finished.set();
                CO_RETURN;
            }

            auto client_transport = rpc::tcp::tcp_transport::create(
                "client_transport", client_service, peer_zone_id, std::chrono::milliseconds(100000), std::move(client), nullptr);

            client_service->spawn(client_transport->pump_send_and_receive());

            rpc::shared_ptr<i_data_processor> remote_service;
            rpc::shared_ptr<i_data_processor> input_service;

            const auto error
                = CO_AWAIT client_service->connect_to_zone("tcp_server", client_transport, input_service, remote_service);

            if (error != rpc::error::OK())
            {
                result.error = error;
                client_finished.set();
                CO_RETURN;
            }

            client_finished.set();

            const auto payload = make_blob(blob_size);
            std::vector<int64_t> durations_us;
            // TCP needs extra warmup calls to eliminate connection establishment overhead
            constexpr size_t tcp_warmup_calls = 100;
            result.error = CO_AWAIT run_benchmark_calls(remote_service, payload, durations_us, tcp_warmup_calls);
            if (result.error == rpc::error::OK())
            {
                result.stats = compute_stats(durations_us);
            }

            remote_service.reset();
            client_transport.reset();
            client_service.reset();
        }

        benchmark_result run_tcp_benchmark(rpc::encoding enc, size_t blob_size, uint16_t port)
        {
            benchmark_result result{};

            auto scheduler_1 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 2},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            auto scheduler_2 = coro::io_scheduler::make_shared(
                coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
                    .pool = coro::thread_pool::options{.thread_count = 2},
                    .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

            rpc::event server_ready;
            rpc::event client_finished;

            coro::sync_wait(coro::when_all(tcp_server_task(scheduler_1, server_ready, client_finished, enc, port),
                tcp_client_task(scheduler_2, server_ready, client_finished, enc, blob_size, port, result)));

            scheduler_1->shutdown();
            scheduler_2->shutdown();

            return result;
        }
#endif

        void print_header()
        {
            fmt::print("Benchmark: 1000 RPC calls per test, middle 80% (drop first/last 10%)\n");
            fmt::print("Warmup: local=10 calls, spsc=20 calls, tcp=100 calls (not included in timing)\n");
            fmt::print("Note: Throughput shown as 'N/A' when avg time < 0.5us (insufficient timing precision)\n");
            fmt::print("Units: MB/s = megabytes per second (1 MB = 1024*1024 bytes)\n");
            fmt::print("-----------------------------------------------------------------------------------------------"
                       "---------------------------------------------\n");
            fmt::print("transport | serialization       | blob bytes | avg (us)     | p50       | p90       | p95      "
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

#ifndef CANOPY_BUILD_COROUTINE
    std::cout << "Benchmark requires CANOPY_BUILD_COROUTINE=ON\n";
    std::cout << "Please configure with: cmake --preset Coroutine_Debug\n";
    return 1;
#else
    using namespace comprehensive::v1;

    const std::vector<encoding_info> encodings = {
        {rpc::encoding::yas_binary, "yas_binary"},
        {rpc::encoding::yas_compressed_binary, "yas_compressed"},
        {rpc::encoding::protocol_buffers, "protocol_buffers"},
        // {rpc::encoding::yas_json, "yas_json"},
    };

    // Test sizes from 64 bytes to 1 MB to investigate throughput scaling
    // Throughput drop at larger sizes may indicate:
    // - Cache effects (L1/L2/L3 cache sizes)
    // - Memory copy overhead becoming dominant
    // - Queue/buffer saturation
    // - Serialization overhead
    const std::vector<size_t> blob_sizes = {
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

    auto local_scheduler = coro::io_scheduler::make_shared(
        coro::io_scheduler::options{.thread_strategy = coro::io_scheduler::thread_strategy_t::spawn,
            .pool = coro::thread_pool::options{.thread_count = 2},
            .execution_strategy = coro::io_scheduler::execution_strategy_t::process_tasks_on_thread_pool});

    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = coro::sync_wait(run_local_benchmark(local_scheduler, enc.enc, blob_size));
            if (result.error != rpc::error::OK())
            {
                fmt::print("local  | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("local", enc.name, blob_size, result.stats);
        }
    }

    local_scheduler->shutdown();

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
    uint16_t tcp_port = 18900;
    for (const auto& enc : encodings)
    {
        for (size_t blob_size : blob_sizes)
        {
            auto result = run_tcp_benchmark(enc.enc, blob_size, tcp_port++);
            if (result.error != rpc::error::OK())
            {
                fmt::print("tcp    | {:>18} | {:>9} | error {}\n", enc.name, blob_size, result.error);
                continue;
            }
            print_stats("tcp", enc.name, blob_size, result.stats);
        }
    }

    print_footer();
    return 0;
#endif
}

void rpc_log(int level, const char* str, size_t sz)
{
    std::string message(str, sz);
    switch (level)
    {
    case 0:
        std::cout << "[DEBUG] " << message << std::endl;
        break;
    case 1:
        std::cout << "[TRACE] " << message << std::endl;
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
