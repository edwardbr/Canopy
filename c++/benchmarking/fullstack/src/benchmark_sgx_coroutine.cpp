/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#if defined(CANOPY_BENCHMARK_SGX_COROUTINE) && defined(CANOPY_BUILD_COROUTINE)

#  include <transports/sgx_coroutine/host/connect.h>
#  include <transports/sgx_coroutine/host/transport.h>

#  include <cstdint>
#  include <memory>
#  include <utility>
#  include <vector>

namespace comprehensive::v1
{
    namespace
    {
        rpc::io_uring::host_controller::options benchmark_enclave_io_uring_options(uint32_t host_buffer_size)
        {
            rpc::io_uring::host_controller::options options;
            options.queue_depth = 256;
            options.use_sqpoll = true;
            options.buffer_count = 256;
            options.buffer_size = host_buffer_size;
            options.register_buffers = false;
            options.fixed_file_count = 128;
            options.register_fixed_files = true;
            return options;
        }

        CORO_TASK(benchmark_result)
        run_sgx_coroutine_io_uring_benchmark_task(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::encoding enc,
            size_t blob_size,
            bool use_proactor,
            uint32_t host_buffer_size)
        {
            benchmark_result result{};

            auto service = rpc::root_service::create("sgx_io_uring_benchmark_host", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            auto transport = std::make_shared<rpc::sgx::coro::host::transport>(
                "sgx_io_uring_benchmark_enclave", service, CANOPY_BENCHMARK_SGX_COROUTINE_ENCLAVE_PATH);

            rpc::shared_ptr<rpc::i_noop> no_host_interface;
            auto connect_result
                = CO_AWAIT rpc::sgx::coro::host::connect_to_enclave_zone<rpc::i_noop, i_enclave_io_uring_benchmark>(
                    service,
                    "sgx_io_uring_benchmark_enclave",
                    transport,
                    no_host_interface,
                    benchmark_enclave_io_uring_options(host_buffer_size));

            auto benchmark = std::move(connect_result.output_interface);
            result.error = connect_result.error_code;
            if (result.error == rpc::error::OK() && !benchmark)
            {
                result.error = rpc::error::OBJECT_NOT_FOUND();
            }

            if (result.error == rpc::error::OK())
            {
                std::vector<uint64_t> enclave_durations;
                result.error = CO_AWAIT benchmark->run_io_uring_rpc(
                    static_cast<uint64_t>(enc),
                    static_cast<uint64_t>(blob_size),
                    static_cast<uint32_t>(call_count),
                    static_cast<uint32_t>(io_uring_warmup_calls),
                    use_proactor,
                    enclave_durations);

                if (result.error == rpc::error::OK())
                {
                    std::vector<int64_t> durations;
                    durations.reserve(enclave_durations.size());
                    for (auto duration : enclave_durations)
                    {
                        durations.push_back(static_cast<int64_t>(duration));
                    }
                    result.stats = compute_stats(durations);
                }
            }

            benchmark = nullptr;
            transport.reset();
            service.reset();
            CO_AWAIT shutdown_event->wait();

            CO_RETURN result;
        }

        CORO_TASK(benchmark_result)
        run_sgx_coroutine_io_uring_pair_benchmark_task(
            std::shared_ptr<coro::scheduler> scheduler,
            rpc::encoding enc,
            size_t blob_size,
            bool use_proactor,
            uint32_t host_buffer_size)
        {
            benchmark_result result{};

            auto service = rpc::root_service::create("sgx_io_uring_pair_benchmark_host", rpc::DEFAULT_PREFIX, scheduler);
            service->set_default_encoding(enc);
            auto shutdown_event = std::make_shared<rpc::event>();
            service->set_shutdown_event(shutdown_event);

            auto server_transport = std::make_shared<rpc::sgx::coro::host::transport>(
                "sgx_io_uring_pair_server_enclave", service, CANOPY_BENCHMARK_SGX_COROUTINE_ENCLAVE_PATH);
            auto client_transport = std::make_shared<rpc::sgx::coro::host::transport>(
                "sgx_io_uring_pair_client_enclave", service, CANOPY_BENCHMARK_SGX_COROUTINE_ENCLAVE_PATH);

            rpc::shared_ptr<rpc::i_noop> no_host_interface;
            auto server_connect
                = CO_AWAIT rpc::sgx::coro::host::connect_to_enclave_zone<rpc::i_noop, i_enclave_io_uring_benchmark>(
                    service,
                    "sgx_io_uring_pair_server_enclave",
                    server_transport,
                    no_host_interface,
                    benchmark_enclave_io_uring_options(host_buffer_size));
            auto server = std::move(server_connect.output_interface);
            result.error = server_connect.error_code;
            if (result.error == rpc::error::OK() && !server)
            {
                result.error = rpc::error::OBJECT_NOT_FOUND();
            }

            rpc::shared_ptr<i_enclave_io_uring_benchmark> client;
            if (result.error == rpc::error::OK())
            {
                auto client_connect
                    = CO_AWAIT rpc::sgx::coro::host::connect_to_enclave_zone<rpc::i_noop, i_enclave_io_uring_benchmark>(
                        service,
                        "sgx_io_uring_pair_client_enclave",
                        client_transport,
                        no_host_interface,
                        benchmark_enclave_io_uring_options(host_buffer_size));
                client = std::move(client_connect.output_interface);
                result.error = client_connect.error_code;
                if (result.error == rpc::error::OK() && !client)
                {
                    result.error = rpc::error::OBJECT_NOT_FOUND();
                }
            }

            bool server_started = false;
            uint32_t port = 0;
            if (result.error == rpc::error::OK())
            {
                result.error = CO_AWAIT server->start_io_uring_rpc_server(static_cast<uint64_t>(enc), use_proactor, port);
                server_started = result.error == rpc::error::OK();
            }

            if (result.error == rpc::error::OK())
            {
                std::vector<uint64_t> enclave_durations;
                result.error = CO_AWAIT client->run_io_uring_rpc_client(
                    static_cast<uint64_t>(enc),
                    static_cast<uint64_t>(blob_size),
                    static_cast<uint32_t>(call_count),
                    static_cast<uint32_t>(io_uring_warmup_calls),
                    use_proactor,
                    port,
                    enclave_durations);

                if (result.error == rpc::error::OK())
                {
                    std::vector<int64_t> durations;
                    durations.reserve(enclave_durations.size());
                    for (auto duration : enclave_durations)
                    {
                        durations.push_back(static_cast<int64_t>(duration));
                    }
                    result.stats = compute_stats(durations);
                }
            }

            if (server_started)
            {
                const auto stop_error = CO_AWAIT server->stop_io_uring_rpc_server();
                if (result.error == rpc::error::OK())
                {
                    result.error = stop_error;
                }
            }

            client = nullptr;
            server = nullptr;
            client_transport.reset();
            server_transport.reset();
            service.reset();
            CO_AWAIT shutdown_event->wait();

            CO_RETURN result;
        }
    } // namespace

    benchmark_result run_sgx_coroutine_io_uring_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        bool use_proactor,
        uint32_t host_buffer_size)
    {
        auto scheduler = make_benchmark_scheduler();
        auto weak_scheduler = std::weak_ptr<coro::scheduler>(scheduler);
        auto result = coro::sync_wait(
            run_sgx_coroutine_io_uring_benchmark_task(scheduler, enc, blob_size, use_proactor, host_buffer_size));

        scheduler->shutdown();
        scheduler.reset();
        wait_for_scheduler_cleanup(weak_scheduler);

        return result;
    }

    benchmark_result run_sgx_coroutine_io_uring_pair_benchmark(
        rpc::encoding enc,
        size_t blob_size,
        bool use_proactor,
        uint32_t host_buffer_size)
    {
        auto scheduler = make_benchmark_scheduler();
        auto weak_scheduler = std::weak_ptr<coro::scheduler>(scheduler);
        auto result = coro::sync_wait(
            run_sgx_coroutine_io_uring_pair_benchmark_task(scheduler, enc, blob_size, use_proactor, host_buffer_size));

        scheduler->shutdown();
        scheduler.reset();
        wait_for_scheduler_cleanup(weak_scheduler);

        return result;
    }
} // namespace comprehensive::v1

#endif
