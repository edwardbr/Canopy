/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include "test_host.h"
#include "common/foo_impl.h"
#include <cstdint>

host::host()
{
#ifdef CANOPY_USE_TELEMETRY
    if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        telemetry_service->on_impl_creation(
            "host",
            reinterpret_cast<std::uintptr_t>(this),
            rpc::service::get_current_service() ? rpc::service::get_current_service()->get_zone_id() : rpc::zone());
#endif
}

host::~host()
{
#ifdef CANOPY_USE_TELEMETRY
    if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        telemetry_service->on_impl_deletion(
            reinterpret_cast<std::uintptr_t>(this),
            rpc::service::get_current_service() ? rpc::service::get_current_service()->get_zone_id() : rpc::zone());
#endif
}

CORO_TASK(error_code) host::create_enclave(rpc::shared_ptr<yyy::i_example>& target)
{
#ifdef CANOPY_BUILD_ENCLAVE
    rpc::shared_ptr<yyy::i_host> host = shared_from_this();
    auto serv = current_host_service.lock();

    auto transport = std::make_shared<rpc::sgx::enclave_transport>("an enclave", serv, enclave_path);

    auto result = CO_AWAIT serv->connect_to_zone<yyy::i_host, yyy::i_example>("an enclave", transport, host);
    target = std::move(result.output_interface);
    CO_RETURN result.error_code;
#endif
    RPC_ERROR("Incompatible service - enclave not built");
    CO_RETURN rpc::error::INCOMPATIBLE_SERVICE();
};

CORO_TASK(error_code) host::create_local_zone(rpc::shared_ptr<yyy::i_example>& target)
{
    rpc::shared_ptr<yyy::i_host> host = shared_from_this();
    auto serv = current_host_service.lock();

    auto child_transport = std::make_shared<rpc::local::child_transport>("main child", serv);
    child_transport->template set_child_entry_point<yyy::i_host, yyy::i_example>(
        [](rpc::shared_ptr<yyy::i_host> host,
            std::shared_ptr<rpc::child_service> child_service_ptr) -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
        {
            auto new_example = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, nullptr));
            CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(new_example)};
        });

    auto connect_result
        = CO_AWAIT serv->template connect_to_zone<yyy::i_host, yyy::i_example>("main child", child_transport, host);
    target = std::move(connect_result.output_interface);
    CO_RETURN connect_result.error_code;
};

// live app registry, it should have sole responsibility for the long term storage of app shared ptrs
CORO_TASK(error_code)
host::look_up_app(
    const std::string& app_name,
    rpc::shared_ptr<yyy::i_example>& app)
{
    {
        std::scoped_lock lock(cached_apps_mux_);
        auto it = cached_apps_.find(app_name);
        if (it != cached_apps_.end())
        {
            app = it->second;
        }
    }
    CO_RETURN rpc::error::OK();
}

CORO_TASK(error_code)
host::set_app(
    const std::string& name,
    const rpc::shared_ptr<yyy::i_example>& app)
{
    {
        std::scoped_lock lock(cached_apps_mux_);
        cached_apps_[name] = app;
    }
    CO_RETURN rpc::error::OK();
}

CORO_TASK(error_code) host::unload_app(const std::string& name)
{
    {
        std::scoped_lock lock(cached_apps_mux_);
        cached_apps_.erase(name);
    }
    CO_RETURN rpc::error::OK();
}
