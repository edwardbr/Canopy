/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Test DLL init function for the shared_scheduler_dll transport tests.

#ifdef CANOPY_BUILD_COROUTINE

#  include <transports/shared_scheduler_dll/dll_transport.h>
#  include <rpc/rpc.h>
#  include <common/tests.h>

#  include <atomic>

namespace
{
    std::atomic<int>& dll_static_probe_counter()
    {
        static std::atomic<int> counter{0};
        return counter;
    }
}

extern "C" CANOPY_SHARED_SCHEDULER_DLL_EXPORT int canopy_shared_scheduler_dll_test_increment_static_probe()
{
    return ++dll_static_probe_counter();
}

namespace rpc::shared_scheduler_dll
{
    // Concrete init coroutine called through the host-scheduled direct coroutine ABI.
    coro::task<rpc::connect_result> canopy_shared_scheduler_dll_init(
        void* transport_ctx,
        const rpc::connection_settings* settings,
        std::shared_ptr<coro::scheduler>* scheduler)
    {
        canopy_shared_scheduler_dll_test_increment_static_probe();
        return init_child_zone<yyy::i_host, yyy::i_example>(
            transport_ctx,
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
