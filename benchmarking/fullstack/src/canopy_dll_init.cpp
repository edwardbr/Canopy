/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <transports/dynamic_library/dll_transport.h>

extern "C" CANOPY_DLL_EXPORT int canopy_dll_init(rpc::dynamic_library::dll_init_params* params)
{
    return rpc::dynamic_library::init_child_zone<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
        params,
        [](rpc::shared_ptr<comprehensive::v1::i_data_processor>,
            std::shared_ptr<rpc::child_service>) -> rpc::service_connect_result<comprehensive::v1::i_data_processor>
        {
            return rpc::service_connect_result<comprehensive::v1::i_data_processor>{
                rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
        });
}

#endif
