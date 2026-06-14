/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#if defined(CANOPY_BUILD_COROUTINE) && defined(FOR_SGX)

#  include <string>
#  include <utility>

#  include <rpc/module.h>
#  include <transports/sgx_coroutine/enclave/runtime.h>

namespace rpc
{
    struct object_module_init_params
    {
        const char* name{};
        bool object_registered{false};
    };

    template<
        class Remote,
        class Local,
        class Factory>
    CORO_TASK(int)
    register_object(
        object_module_init_params* module_params,
        Factory&& factory)
    {
        if (!module_params)
            CO_RETURN rpc::error::INVALID_DATA();

        module_params->object_registered = true;
        auto object_name = module_params->name ? std::string{module_params->name} : std::string{};
        auto typed_factory = rpc::module::make_object_factory<Remote, Local>(std::forward<Factory>(factory));
        rpc::sgx_coroutine_transport::enclave::register_connection_factory<Remote, Local>(
            object_name,
            [object_name = std::move(object_name), typed_factory = std::move(typed_factory)](
                rpc::shared_ptr<Remote> remote,
                std::shared_ptr<rpc::service> service) mutable -> CORO_TASK(rpc::service_connect_result<Local>)
            {
                const auto& applications = rpc::sgx_coroutine_transport::enclave::runtime_startup_applications();
                auto context = rpc::module::make_object_factory_context(object_name, nullptr, &applications);
                CO_RETURN CO_AWAIT typed_factory(std::move(remote), std::move(service), context);
            });
        CO_RETURN rpc::error::OK();
    }
}

#endif
