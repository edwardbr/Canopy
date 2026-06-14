/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Test DLL init function for the shared_scheduler_dll transport tests.

#ifdef CANOPY_BUILD_COROUTINE

#  include <common/tests.h>
#  include <rpc_objects/object_registration.h>

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

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params)
{
    canopy_shared_scheduler_dll_test_increment_static_probe();
    CO_RETURN CO_AWAIT rpc::register_object<yyy::i_host, yyy::i_example>(
        params,
        [](rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::service> svc,
            rpc::module::object_factory_context) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            CO_RETURN rpc::service_connect_result<yyy::i_example>{
                rpc::error::OK(), rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host))};
        });
}

#endif // CANOPY_BUILD_COROUTINE
