/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/ipc_transport/transport.h>

namespace comprehensive::v1
{
    CORO_TASK(benchmark_result)
    run_ipc_direct_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size)
    {
        benchmark_result result{};
        constexpr size_t ipc_child_scheduler_thread_count = 1;

        auto root_service = rpc::root_service::create("benchmark_ipc_direct", rpc::DEFAULT_PREFIX, scheduler);
        root_service->set_default_encoding(enc);

        auto child_zone = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] auto ok = child_zone.set_subnet(child_zone.get_subnet() + 1);
        RPC_ASSERT(ok);

        auto transport = rpc::ipc_transport::make_client(
            "benchmark_ipc_direct",
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
    run_ipc_dll_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size)
    {
        benchmark_result result{};
        constexpr size_t ipc_child_scheduler_thread_count = 1;

        auto root_service = rpc::root_service::create("benchmark_ipc_dll", rpc::DEFAULT_PREFIX, scheduler);
        root_service->set_default_encoding(enc);

        auto child_zone = rpc::DEFAULT_PREFIX;
        [[maybe_unused]] auto ok = child_zone.set_subnet(child_zone.get_subnet() + 1);
        RPC_ASSERT(ok);

        auto transport = rpc::ipc_transport::make_client(
            "benchmark_ipc_dll",
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
}

#endif
