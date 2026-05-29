/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <connection_factory/options.h>

namespace rpc::connection_factory
{
    // Service wiring is shared by stream-only and RPC factories. A service may
    // be supplied by the application when it already has a root service, or
    // created here when the caller only asked for a stream/RPC connection. JSON
    // configuration never carries service pointers; it only controls names and
    // serialisation defaults.

    // Factory helpers need an executor even when the caller is only creating a
    // stream and has not provided a service. In coroutine builds this creates a
    // scheduler; in blocking builds it creates the normal executor.
    inline rpc::executor_ptr make_default_executor()
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

    // Apply service-level options that can be changed on an existing service.
    // Ownership stays with the caller when a service is passed into a factory.
    inline int configure_service(
        const std::shared_ptr<rpc::service>& service,
        const rpc::connection_factory_config::stream_factory_options& options)
    {
        if (!service)
            return rpc::error::OK();
        if (auto enc = encoding_option(options))
            service->set_default_encoding(*enc);
        return rpc::error::OK();
    }

    // Return the caller's service when provided, otherwise create a root service
    // using the configured or default name. The shared_ptr is deliberately a
    // std::shared_ptr because these helpers sit outside marshalled IDL ownership.
    inline std::shared_ptr<rpc::service> ensure_service(
        const rpc::connection_factory_config::stream_factory_options& options,
        std::shared_ptr<rpc::service> service,
        std::string default_name)
    {
        if (service)
        {
            configure_service(service, options);
            return service;
        }

        rpc::service_config config;
        const auto name = configured_name(options, "service", std::move(default_name));
        auto created = rpc::root_service::create(name.c_str(), config, make_default_executor());
        configure_service(created, options);
        return created;
    }
} // namespace rpc::connection_factory
