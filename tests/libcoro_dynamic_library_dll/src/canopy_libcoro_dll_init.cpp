/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Test DLL init function for the libcoro_dynamic_library transport tests.

#ifdef CANOPY_BUILD_COROUTINE

#include <transports/libcoro_dynamic_library/dll_transport.h>
#include <rpc/rpc.h>
#include <common/tests.h>

namespace rpc::libcoro_dynamic_library
{
    // Concrete init coroutine: signature matches dll_coro_init_fn.
    // Called by the host's inner_connect after canopy_libcoro_dll_create.
    coro::task<rpc::connect_result> canopy_libcoro_dll_init(
        void* transport_ctx, const rpc::connection_settings* settings, std::shared_ptr<coro::scheduler>* scheduler)
    {
        return init_child_zone<yyy::i_host, yyy::i_example>(transport_ctx,
            settings,
            scheduler,
            [](rpc::shared_ptr<yyy::i_host> host,
                std::shared_ptr<rpc::child_service> svc) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                CO_RETURN rpc::service_connect_result<yyy::i_example>{
                    rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host))};
            });
    }
}

#endif // CANOPY_BUILD_COROUTINE
