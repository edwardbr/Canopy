/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifdef CANOPY_BUILD_COROUTINE

#  include <functional>
#  include <memory>
#  include <string>
#  include <utility>

#  include <json/convert.h>
#  include <rpc/module.h>
#  include <transports/shared_scheduler_dll/dll_transport.h>

namespace rpc
{
    struct object_module_init_params
    {
        void* transport_ctx{};
        const rpc::connection_settings* settings{};
        std::shared_ptr<coro::scheduler>* scheduler{};
        std::function<CORO_TASK(rpc::connect_result)()> init_selected_object;
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
        if (!module_params || !module_params->settings || !module_params->scheduler)
            CO_RETURN rpc::error::INVALID_DATA();

        if (module_params->settings->inbound_interface_id != Remote::get_id(rpc::get_version())
            || module_params->settings->outbound_interface_id != Local::get_id(rpc::get_version()))
            CO_RETURN rpc::error::OK();

        auto* parent = static_cast<rpc::shared_scheduler_dll::parent_transport*>(module_params->transport_ctx);
        if (!parent)
            CO_RETURN rpc::error::INVALID_DATA();

        module_params->object_registered = true;
        auto name = parent->get_name();
        std::shared_ptr<json::v1::object> module_global_settings;
        std::shared_ptr<rpc::module::startup_applications> startup_applications;
        const auto& module_settings_json = parent->module_settings_json();
        if (!module_settings_json.empty())
        {
            try
            {
                module_global_settings = std::make_shared<json::v1::object>(json::v1::parse(module_settings_json));
            }
            catch (...)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }
        }
        const auto& startup_applications_json = parent->startup_applications_json();
        if (!startup_applications_json.empty())
        {
            try
            {
                startup_applications = std::make_shared<rpc::module::startup_applications>(
                    json::v1::convert::from_json_object<rpc::module::startup_applications>(
                        json::v1::parse(startup_applications_json)));
            }
            catch (...)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }
        }

        auto typed_factory = rpc::module::make_object_factory<Remote, Local>(std::forward<Factory>(factory));
        module_params->init_selected_object
            = [transport_ctx = module_params->transport_ctx,
                  settings = module_params->settings,
                  scheduler = module_params->scheduler,
                  name = std::move(name),
                  module_global_settings = std::move(module_global_settings),
                  startup_applications = std::move(startup_applications),
                  typed_factory = std::move(typed_factory)]() mutable -> CORO_TASK(rpc::connect_result)
        {
            auto wrapped_factory = rpc::module::make_child_service_factory<Remote, Local>(
                name, module_global_settings.get(), typed_factory, startup_applications.get());
            CO_RETURN CO_AWAIT rpc::shared_scheduler_dll::init_child_zone<Remote, Local>(
                transport_ctx, settings, scheduler, std::move(wrapped_factory));
        };
        CO_RETURN rpc::error::OK();
    }
}

#endif
