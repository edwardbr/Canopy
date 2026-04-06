/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <transports/c_abi/dll_transport.h>

#include <common/tests.h>
#include <rpc/rpc.h>

extern "C" CANOPY_C_ABI_EXPORT int32_t canopy_dll_init(canopy_dll_init_params* params)
{
    return rpc::c_abi::init_child_zone<yyy::i_host, yyy::i_example>(
        params,
        [](rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::child_service> svc) -> rpc::service_connect_result<yyy::i_example>
        {
            auto impl = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host));
            return rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(impl)};
        });
}
