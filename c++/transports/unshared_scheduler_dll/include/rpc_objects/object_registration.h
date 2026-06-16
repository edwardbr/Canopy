/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <transports/unshared_scheduler_dll/object_registration.h>

// Transport-selected object-module adapter.
//
// An unshared-scheduler DLL module defines canopy_module_init() and calls
// rpc::register_object<Remote, Local>(). This header maps that uniform module
// hook onto the DLL-owned scheduler runtime ABI.
static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params);

namespace rpc::unshared_scheduler_dll
{
    CORO_TASK(rpc::connect_result)
    canopy_unshared_scheduler_dll_init(
        void* transport_ctx,
        const rpc::connection_settings* settings)
    {
        rpc::object_module_init_params params{transport_ctx, settings, {}, false};
        const auto error_code = CO_AWAIT ::canopy_module_init(&params);
        if (error_code != rpc::error::OK())
            CO_RETURN rpc::connect_result{error_code, {}};

        if (!params.object_registered || !params.init_selected_object)
            CO_RETURN rpc::connect_result{rpc::error::INVALID_INTERFACE_ID(), {}};

        CO_RETURN CO_AWAIT params.init_selected_object();
    }
}
