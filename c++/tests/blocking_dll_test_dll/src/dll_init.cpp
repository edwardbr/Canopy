/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <common/tests.h>
#include <rpc_objects/object_registration.h>

static CORO_TASK(int) canopy_module_init(rpc::object_module_init_params* params)
{
    CO_RETURN CO_AWAIT rpc::register_object<yyy::i_host, yyy::i_example>(
        params,
        [](rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::service> svc,
            rpc::module::object_factory_context) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            auto impl = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(svc, host));
            CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(impl)};
        });
}
