/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <transports/sgx_coroutine/enclave/object_registration.h>

// Transport-selected object-module adapter.
//
// A coroutine SGX object module defines canopy_module_init() and calls
// rpc::register_object<Remote, Local>(). This header runs that uniform module
// hook during enclave static initialization and registers the selected object
// factory with the SGX coroutine runtime.
#ifndef CANOPY_RPC_OBJECT_MODULE_NAME
#  define CANOPY_RPC_OBJECT_MODULE_NAME ""
#endif

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params);

namespace rpc_objects::detail
{
    struct object_module_auto_registration
    {
        object_module_auto_registration()
        {
            rpc::object_module_init_params params{CANOPY_RPC_OBJECT_MODULE_NAME};
            const auto error_code = SYNC_WAIT(::canopy_module_init(&params));
            if (error_code != rpc::error::OK())
                RPC_ERROR("RPC object module registration failed: {}", error_code);
            else if (!params.object_registered)
                RPC_ERROR("RPC object module registered no interface factories");
        }
    };

    [[maybe_unused]] const object_module_auto_registration object_module_auto_registration_instance{};
}
