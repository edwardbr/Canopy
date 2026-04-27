/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <rpc/rpc.h>
#include <common/foo_impl.h>
#include <transports/sgx_coroutine/host/runtime.h>

namespace
{
    struct connection_factory_registrar
    {
        connection_factory_registrar()
        {
            rpc::sgx::coro::host::register_connection_factory<yyy::i_host, yyy::i_example>(
                "marshal_test_coroutine_enclave",
                [](rpc::shared_ptr<yyy::i_host> host,
                    std::shared_ptr<rpc::service> child_service) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
                {
                    rpc::shared_ptr<yyy::i_example> example(new marshalled_tests::example(child_service, host));
                    CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(example)};
                });
        }
    };

    connection_factory_registrar g_connection_factory_registrar;
}
