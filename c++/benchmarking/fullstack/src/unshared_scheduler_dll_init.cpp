/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifdef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <transports/unshared_scheduler_dll/dll_transport.h>

namespace rpc::unshared_scheduler_dll
{
    coro::task<rpc::connect_result> canopy_unshared_scheduler_dll_init(
        void* transport_ctx,
        const rpc::connection_settings* settings)
    {
        return init_child_zone<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
            transport_ctx,
            settings,
            [](rpc::shared_ptr<comprehensive::v1::i_data_processor>, std::shared_ptr<rpc::child_service>)
                -> CORO_TASK(rpc::service_connect_result<comprehensive::v1::i_data_processor>)
            {
                CO_RETURN rpc::service_connect_result<comprehensive::v1::i_data_processor>{
                    rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
            });
    }
}

#endif
