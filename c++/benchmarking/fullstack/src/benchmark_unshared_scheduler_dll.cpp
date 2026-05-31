/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "benchmark_common.h"

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/unshared_scheduler_dll/transport.h>

namespace comprehensive::v1
{
    CORO_TASK(benchmark_result)
    run_unshared_scheduler_dll_benchmark(
        std::shared_ptr<coro::scheduler> scheduler,
        rpc::encoding enc,
        size_t blob_size)
    {
        benchmark_result result{};

        auto root_service = rpc::root_service::create("benchmark_unshared_scheduler_dll", rpc::DEFAULT_PREFIX, scheduler);
        root_service->set_default_encoding(enc);

        auto child_transport = std::make_shared<rpc::unshared_scheduler_dll::child_transport>(
            "benchmark_unshared_scheduler_dll", root_service, CANOPY_BENCHMARK_UNSHARED_SCHEDULER_DLL_PATH);

        rpc::shared_ptr<i_data_processor> remote_processor;
        rpc::shared_ptr<i_data_processor> not_used;

        {
            auto connect_result = CO_AWAIT root_service->connect_to_zone<i_data_processor, i_data_processor>(
                "benchmark_unshared_scheduler_dll", child_transport, not_used);
            remote_processor = connect_result.output_interface;
            result.error = connect_result.error_code;
            if (result.error != rpc::error::OK())
                CO_RETURN result;
        }
        not_used = nullptr;
        child_transport.reset();
        root_service.reset();

        const auto payload = make_blob(blob_size);
        std::vector<int64_t> durations_ns;
        result.error = CO_AWAIT run_benchmark_calls(remote_processor, payload, durations_ns, dll_warmup_calls);
        if (result.error == rpc::error::OK())
            result.stats = compute_stats(durations_ns);

        remote_processor = nullptr;
        CO_RETURN result;
    }
}

#endif
