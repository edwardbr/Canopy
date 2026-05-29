/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_uring.h"

#include <rpc/rpc.h>
#include <example/example.h>
#include <io_uring/controller.h>
#include <secure_coroutine_module/secure_coroutine_module.h>
#include <io_uring_test/test.h>
#include <transports/sgx_coroutine/enclave/runtime.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace
{
    void exercise_atomic_smart_ptr_polyfill()
    {
        std::atomic<std::shared_ptr<int>> std_ptr;
        std_ptr.store(nullptr);
        auto std_loaded = std_ptr.load();
        (void)std_loaded;

        std::atomic<rpc::shared_ptr<io_uring_test::i_test_uring>> rpc_shared_ptr;
        rpc_shared_ptr.store(nullptr);
        auto rpc_shared_loaded = rpc_shared_ptr.load();
        (void)rpc_shared_loaded;

        std::atomic<rpc::optimistic_ptr<io_uring_test::i_test_uring>> rpc_optimistic_ptr;
        rpc_optimistic_ptr.store(nullptr);
        auto rpc_optimistic_loaded = rpc_optimistic_ptr.load();
        (void)rpc_optimistic_loaded;
    }

    struct enclave_entry_point
    {
        enclave_entry_point()
        {
            exercise_atomic_smart_ptr_polyfill();

            rpc::sgx::coro::enclave::register_connection_factory<yyy::i_host, io_uring_test::i_test_uring>(
                "sgx_coroutine_test_enclave",
                [](rpc::shared_ptr<yyy::i_host> host, std::shared_ptr<rpc::service> child_service)
                    -> CORO_TASK(rpc::service_connect_result<io_uring_test::i_test_uring>)
                {
                    try
                    {
                        if (!child_service)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        if (!host)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        auto enclave_service = std::dynamic_pointer_cast<rpc::enclave_service>(child_service);
                        if (!enclave_service)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        auto controller = enclave_service->get_io_uring_controller();
                        if (!controller)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        rpc::shared_ptr<io_uring_test::i_test_uring> test(
                            new io_uring_test_enclave::test_uring(controller, std::move(child_service)));
                        if (!test)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::OUT_OF_MEMORY(), {}};
                        }

                        CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                            rpc::error::OK(), std::move(test)};
                    }
                    catch (const std::bad_alloc&)
                    {
                        CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{rpc::error::OUT_OF_MEMORY(), {}};
                    }
                    catch (...)
                    {
                        CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{rpc::error::EXCEPTION(), {}};
                    }
                });
        }
    };

    enclave_entry_point g_enclave_entry_point;
}
