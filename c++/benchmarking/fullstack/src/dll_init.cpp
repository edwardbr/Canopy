/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#ifndef CANOPY_BUILD_COROUTINE

#  include "benchmark_data_processor.h"
#  include <rpc_objects/object_registration.h>

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params)
{
    CO_RETURN CO_AWAIT rpc::register_object<comprehensive::v1::i_data_processor, comprehensive::v1::i_data_processor>(
        params,
        [](rpc::shared_ptr<comprehensive::v1::i_data_processor>,
            std::shared_ptr<rpc::service>,
            rpc::module::object_factory_context) -> CORO_TASK(rpc::service_connect_result<comprehensive::v1::i_data_processor>)
        {
            CO_RETURN rpc::service_connect_result<comprehensive::v1::i_data_processor>{
                rpc::error::OK(), comprehensive::v1::make_benchmark_data_processor()};
        });
}

#endif
