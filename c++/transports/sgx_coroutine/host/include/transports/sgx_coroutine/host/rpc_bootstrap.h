/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>
#include <string>

#include <rpc/rpc.h>
#include <secure_coroutine_module/secure_coroutine_module.h>

namespace rpc::sgx_coroutine_transport::host
{
    struct rpc_bootstrap_context
    {
        int error_code{rpc::error::OK()};
        std::shared_ptr<rpc::service> service;
        std::shared_ptr<rpc::transport> transport;
        std::string service_proxy_name;
        rpc::shared_ptr<rpc::v4::secure_coroutine_module::i_io_uring_control> control;
    };

    template<class Out> CORO_TASK(rpc::service_connect_result<Out>) connect_rpc_transport(rpc_bootstrap_context context)
    {
        if (context.error_code != rpc::error::OK())
            CO_RETURN rpc::service_connect_result<Out>{context.error_code, {}};
        if (!context.service || !context.transport || !context.control)
            CO_RETURN rpc::service_connect_result<Out>{rpc::error::INVALID_DATA(), {}};

        CO_RETURN CO_AWAIT context.service->template connect_to_zone<rpc::v4::secure_coroutine_module::i_io_uring_control, Out>(
            context.service_proxy_name.c_str(), std::move(context.transport), std::move(context.control));
    }
} // namespace rpc::sgx_coroutine_transport::host
