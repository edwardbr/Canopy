/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

// Standard C++ headers
#include <atomic>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <tuple>

// RPC headers
#include <rpc/rpc.h>
#ifdef CANOPY_USE_TELEMETRY
#  include <rpc/telemetry/i_telemetry_service.h>
#endif

#include <common/foo_impl.h>
#include <example/example.h>
#include "test_globals.h"

template<class Derived, class... ExtraInterfaces>
class basic_host : public rpc::base<Derived, yyy::i_host, ExtraInterfaces...>,
                   public rpc::enable_shared_from_this<Derived>
{
    // perhaps this should be an unsorted list but map is easier to debug for now
    std::map<std::string, rpc::shared_ptr<yyy::i_example>> cached_apps_;
    std::mutex cached_apps_mux_;

    rpc::shared_ptr<yyy::i_host> self_host() { return rpc::static_pointer_cast<yyy::i_host>(this->shared_from_this()); }

public:
    ~basic_host() override = default;

    CORO_TASK(error_code) create_local_zone(rpc::shared_ptr<yyy::i_example>& target) override
    {
        auto serv = current_host_service.lock();

        auto child_transport = std::make_shared<rpc::local::child_transport>("main child", serv);
        child_transport->template set_child_entry_point<yyy::i_host, yyy::i_example>(
            [](rpc::shared_ptr<yyy::i_host> host, std::shared_ptr<rpc::child_service> child_service_ptr)
                -> CORO_TASK(rpc::service_connect_result<yyy::i_example>)
            {
                auto new_example
                    = rpc::shared_ptr<yyy::i_example>(new marshalled_tests::example(child_service_ptr, nullptr));
                CO_RETURN rpc::service_connect_result<yyy::i_example>{rpc::error::OK(), std::move(new_example)};
            });

        auto connect_result = CO_AWAIT serv->template connect_to_zone<yyy::i_host, yyy::i_example>(
            "main child", child_transport, self_host());
        target = std::move(connect_result.output_interface);
        CO_RETURN connect_result.error_code;
    };

    CORO_TASK(error_code)
    look_up_app(
        const std::string& app_name,
        rpc::shared_ptr<yyy::i_example>& app) override
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
    set_app(
        const std::string& name,
        const rpc::shared_ptr<yyy::i_example>& app) override
    {
        {
            std::scoped_lock lock(cached_apps_mux_);
            cached_apps_[name] = app;
        }
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(error_code) unload_app(const std::string& name) override
    {
        {
            std::scoped_lock lock(cached_apps_mux_);
            cached_apps_.erase(name);
        }
        CO_RETURN rpc::error::OK();
    }
};

class host : public basic_host<host>
{
public:
    host()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_impl_creation(
                {"host",
                    reinterpret_cast<std::uintptr_t>(this),
                    rpc::service::get_current_service() ? rpc::service::get_current_service()->get_zone_id() : rpc::zone()});
        }
#endif
    }

    ~host() override
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_impl_deletion(
                {reinterpret_cast<std::uintptr_t>(this),
                    rpc::service::get_current_service() ? rpc::service::get_current_service()->get_zone_id() : rpc::zone()});
        }
#endif
    }
};
