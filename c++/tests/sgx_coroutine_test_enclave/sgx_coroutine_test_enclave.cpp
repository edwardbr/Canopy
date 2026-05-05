/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_uring.h"

#include <rpc/rpc.h>
#include <example/example.h>
#include <io_uring/controller.h>
#include <io_uring/io_uring.h>
#include <io_uring_test/test.h>
#include <transports/sgx_coroutine/host/runtime.h>

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

    struct connection_factory_registrar
    {
        connection_factory_registrar()
        {
            exercise_atomic_smart_ptr_polyfill();

            rpc::sgx::coro::host::register_connection_factory<yyy::i_host, io_uring_test::i_test_uring>(
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

                        auto scheduler = child_service->get_scheduler();
                        if (!scheduler)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        auto host_io_uring
                            = CO_AWAIT rpc::dynamic_pointer_cast<rpc::io_uring::i_host_io_uring_control>(host);
                        if (!host_io_uring)
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{
                                rpc::error::INCOMPATIBLE_SERVICE(), {}};
                        }

                        // create a controller
                        std::shared_ptr<rpc::io_uring::controller> controller;
                        controller
                            = std::make_shared<rpc::io_uring::controller>(std::move(host_io_uring), scheduler.get());

                        // register cleanup
                        std::weak_ptr<rpc::io_uring::controller> runtime_controller = controller;
                        auto err = rpc::sgx::coro::host::register_runtime_cleanup_handler(
                            [runtime_controller]() noexcept
                            {
                                if (auto controller = runtime_controller.lock())
                                    controller->request_shutdown();
                            });
                        if (err != rpc::error::OK())
                        {
                            CO_RETURN rpc::service_connect_result<io_uring_test::i_test_uring>{err, {}};
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

    connection_factory_registrar g_connection_factory_registrar;
}
