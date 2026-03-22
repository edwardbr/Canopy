/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#include "benchmark_data_processor.h"
#include <transports/local/transport.h>

#ifndef CANOPY_BUILD_COROUTINE
#  include <transports/dynamic_library/transport.h>
#endif

namespace comprehensive::v1
{
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
            [](const rpc::shared_ptr<i_data_processor>&,
                const std::shared_ptr<rpc::child_service>&) -> CORO_TASK(rpc::service_connect_result<i_data_processor>)
            {
                CO_RETURN rpc::service_connect_result<i_data_processor>{rpc::error::OK(), make_benchmark_data_processor()};
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
            result.stats = compute_stats(durations_us);

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
}
