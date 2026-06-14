/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#if !defined(CANOPY_BUILD_COROUTINE) && defined(FOR_SGX)

#  include <cstddef>
#  include <memory>
#  include <string>
#  include <utility>

#  include <rpc/module.h>
#  include <transports/sgx_blocking/object_runtime.h>

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
        auto name = module_params->name ? std::string{module_params->name} : std::string{};
        auto child_service_name = name;
        auto wrapped_factory = rpc::module::make_object_factory<Remote, Local>(std::forward<Factory>(factory));
        rpc::sgx_blocking_transport::object_runtime::register_connection_factory(
            std::move(name),
            Remote::get_id(rpc::get_version()),
            Local::get_id(rpc::get_version()),
            [wrapped_factory = std::move(wrapped_factory), child_service_name = std::move(child_service_name)](
                const rpc::sgx_blocking_transport::object_runtime::detail::init_request& request,
                std::shared_ptr<rpc::sgx_blocking_transport::host_transport> host_transport,
                size_t resp_cap,
                char* resp,
                size_t* resp_sz) -> int
            {
                namespace object_runtime = rpc::sgx_blocking_transport::object_runtime;

                const auto input_descr = object_runtime::detail::from_sgx_init_request(request);
                auto child_factory = rpc::module::make_child_service_factory<Remote, Local>(
                    child_service_name, nullptr, wrapped_factory, &request.applications);
                auto result = rpc::child_service::create_child_zone<Remote, Local>(
                    child_service_name, std::move(host_transport), input_descr, std::move(child_factory), rpc::executor_ptr{});

                if (result.error_code != rpc::error::OK())
                    return object_runtime::detail::write_blob_response(
                        object_runtime::detail::to_sgx_init_response(result.error_code, {}), resp_cap, resp, resp_sz);

                return object_runtime::detail::write_blob_response(
                    object_runtime::detail::to_sgx_init_response(rpc::error::OK(), result.descriptor), resp_cap, resp, resp_sz);
            });
        CO_RETURN rpc::error::OK();
    }
}

#endif
