/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <transports/blocking_dll/dll_transport.h>

extern "C" CANOPY_DLL_EXPORT int canopy_dll_init(rpc::blocking_dll::dll_init_params* params)
{
    return rpc::blocking_dll::init_child_zone<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
        params,
        [](rpc::shared_ptr<comprehensive::v1::i_data_processor>,
            std::shared_ptr<rpc::child_service>) -> rpc::service_connect_result<comprehensive::v1::i_data_processor>
        {
            return rpc::service_connect_result<comprehensive::v1::i_data_processor>{
                rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
        });
}

#endif
