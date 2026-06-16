/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <transports/blocking_dll/object_registration.h>

// Transport-selected object-module adapter.
//
// A blocking DLL module defines canopy_module_init() and calls
// rpc::register_object<Remote, Local>(). This header maps that uniform module
// hook onto the blocking DLL ABI by supplying canopy_dll_init().
static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params);

extern "C" CANOPY_DLL_EXPORT int canopy_dll_init(rpc::blocking_dll::dll_init_params* params)
{
    rpc::object_module_init_params module_params{params};
    const auto error_code = ::canopy_module_init(&module_params);
    if (error_code != rpc::error::OK())
        return error_code;
    if (!module_params.object_registered)
        return rpc::error::INVALID_INTERFACE_ID();
    return rpc::error::OK();
}
