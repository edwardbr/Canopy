/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <common/foo_impl.h>
#include <transports/sgx_coroutine/enclave/runtime.h>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <io_uring/controller.h>

namespace
{
    void exercise_atomic_smart_ptr_polyfill()
    {
        std::atomic<std::shared_ptr<int>> std_ptr;
        std_ptr.store(nullptr);
        auto std_loaded = std_ptr.load();
        (void)std_loaded;

        std::atomic<rpc::shared_ptr<yyy::i_example>> rpc_shared_ptr;
        rpc_shared_ptr.store(nullptr);
        auto rpc_shared_loaded = rpc_shared_ptr.load();
        (void)rpc_shared_loaded;

        std::atomic<rpc::optimistic_ptr<yyy::i_example>> rpc_optimistic_ptr;
        rpc_optimistic_ptr.store(nullptr);
        auto rpc_optimistic_loaded = rpc_optimistic_ptr.load();
        (void)rpc_optimistic_loaded;
    }

    struct enclave_entry_point
    {
        enclave_entry_point()
        {
            exercise_atomic_smart_ptr_polyfill();

            rpc::sgx::coro::enclave::register_connection_factory<yyy::i_host, yyy::i_example>(
                "marshal_test_coroutine_enclave",
                [](rpc::shared_ptr<yyy::i_host> host,
                    std::shared_ptr<rpc::service> child_service) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
                {
                    rpc::shared_ptr<yyy::i_example> example(new marshalled_tests::example(child_service, host));
                    CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
                });
        }
    };

    enclave_entry_point g_enclave_entry_point;
}
