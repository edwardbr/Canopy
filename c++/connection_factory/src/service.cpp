/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <connection_factory/service.h>

#include <algorithm>
#include <thread>
#include <utility>

namespace rpc::connection_factory
{
    rpc::executor_ptr make_default_executor()
    {
#ifdef CANOPY_BUILD_COROUTINE
        auto options = rpc::coro::scheduler::options{};
        options.thread_strategy = rpc::coro::scheduler::thread_strategy_t::spawn;
        options.pool.thread_count = std::max(1U, std::thread::hardware_concurrency());
        return rpc::coro::make_shared_scheduler(options);
#else
        return std::make_shared<rpc::executor>();
#endif
    }

    int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const rpc::stream_transport::transport_settings& settings)
    {
        if (!service)
            return rpc::error::OK();
        if (auto enc = encoding_option(settings))
            service->set_default_encoding(*enc);
        return rpc::error::OK();
    }

    std::shared_ptr<rpc::service> ensure_service(
        const rpc::connection_factory_config::service::settings& service_settings,
        const rpc::stream_transport::transport_settings& transport_settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        if (service)
        {
            configure_service(service, transport_settings);
            return service;
        }

        rpc::service_config config;
        const auto name = service_name(service_settings, std::move(default_name));
        auto created = rpc::root_service::create(name.c_str(), config, make_default_executor());
        configure_service(created, transport_settings);
        return created;
    }

    std::shared_ptr<rpc::service> ensure_service(
        const stream_rpc_connection_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return ensure_service(settings.service, settings.transport, std::move(service), std::move(default_name));
    }

    std::shared_ptr<rpc::service> ensure_service(
        const rpc::stream_transport::transport_settings& settings,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        return ensure_service({}, settings, std::move(service), std::move(default_name));
    }
} // namespace rpc::connection_factory
