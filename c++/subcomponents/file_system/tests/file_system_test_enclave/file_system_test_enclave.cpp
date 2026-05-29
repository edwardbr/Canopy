/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <file_system/file_system_manager.h>
#include <rpc/rpc.h>

#include <secure_coroutine_module/secure_coroutine_module.h>
#include <transports/sgx_coroutine/enclave/runtime.h>

#include <atomic>
#include <memory>

namespace
{
    struct enclave_entry_point
    {
        enclave_entry_point()
        {

            rpc::sgx::coro::enclave::register_connection_factory<rpc::i_noop, rpc::file_system::i_manager>(
                "file_system_test_enclave",
                [](rpc::shared_ptr<rpc::i_noop>, std::shared_ptr<rpc::service> child_service)
                    -> CORO_TASK(rpc::service_connect_result<rpc::file_system::i_manager>)
                {
                    if (!child_service)
                    {
                        CO_RETURN rpc::service_connect_result<rpc::file_system::i_manager>{
                            rpc::error::INCOMPATIBLE_SERVICE(), {}};
                    }

                    auto enclave_service = std::dynamic_pointer_cast<rpc::enclave_service>(child_service);
                    if (!enclave_service)
                    {
                        CO_RETURN rpc::service_connect_result<rpc::file_system::i_manager>{
                            rpc::error::INCOMPATIBLE_SERVICE(), {}};
                    }

                    auto controller = enclave_service->get_io_uring_controller();
                    if (!controller)
                    {
                        CO_RETURN rpc::service_connect_result<rpc::file_system::i_manager>{
                            rpc::error::INCOMPATIBLE_SERVICE(), {}};
                    }

                    auto manager = rpc::file_system::create_factory(std::move(controller));
                    if (!manager)
                    {
                        CO_RETURN rpc::service_connect_result<rpc::file_system::i_manager>{rpc::error::OUT_OF_MEMORY(), {}};
                    }

                    CO_RETURN rpc::service_connect_result<rpc::file_system::i_manager>{
                        rpc::error::OK(), std::move(manager)};
                });
        }
    };

    enclave_entry_point g_enclave_entry_point;
}
