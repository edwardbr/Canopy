/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Test DLL entry point for the dynamic_library transport tests.
//
// This shared object is loaded by dll_transport_setup via dlopen.
// It wires yyy::i_host (host side) to yyy::i_example (DLL side) using
// marshalled_tests::example as the implementation.

#include <transports/dynamic_library/dll_transport.h>
#include <rpc/rpc.h>
#include <common/tests.h>

extern "C" CANOPY_DLL_EXPORT int canopy_dll_init(rpc::dynamic_library::dll_init_params* params)
{
    return rpc::dynamic_library::init_child_zone<yyy::i_host, yyy::i_example>(
        params,
        [](rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::child_service> svc) -> rpc::service_connect_result<yyy::i_example>
        {
            auto impl = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host));
            return rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(impl)};
        });
}
