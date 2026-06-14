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
#  include <transports/unshared_scheduler_dll/dll_transport.h>

namespace rpc
{
    struct object_module_init_params
    {
        void* transport_ctx{};
        const rpc::connection_settings* settings{};
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
        if (!module_params || !module_params->settings)
            CO_RETURN rpc::error::INVALID_DATA();

        if (module_params->settings->inbound_interface_id != Remote::get_id(rpc::get_version())
            || module_params->settings->outbound_interface_id != Local::get_id(rpc::get_version()))
            CO_RETURN rpc::error::OK();

        auto* runtime = static_cast<rpc::unshared_scheduler_dll::runtime_context*>(module_params->transport_ctx);
        if (!runtime || !runtime->pending_transport)
            CO_RETURN rpc::error::INVALID_DATA();

        module_params->object_registered = true;
        auto name = runtime->pending_transport->get_name();
        std::shared_ptr<json::v1::object> module_global_settings;
        if (!runtime->module_settings_json.empty())
        {
            try
            {
                module_global_settings
                    = std::make_shared<json::v1::object>(json::v1::parse(runtime->module_settings_json));
            }
            catch (...)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }
        }
        std::shared_ptr<rpc::module::startup_applications> startup_applications;
        if (!runtime->startup_applications_json.empty())
        {
            try
            {
                startup_applications = std::make_shared<rpc::module::startup_applications>(
                    json::v1::convert::from_json_object<rpc::module::startup_applications>(
                        json::v1::parse(runtime->startup_applications_json)));
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
                  name = std::move(name),
                  module_global_settings = std::move(module_global_settings),
                  startup_applications = std::move(startup_applications),
                  typed_factory = std::move(typed_factory)]() mutable -> CORO_TASK(rpc::connect_result)
        {
            auto wrapped_factory = rpc::module::make_child_service_factory<Remote, Local>(
                name, module_global_settings.get(), typed_factory, startup_applications.get());
            CO_RETURN CO_AWAIT rpc::unshared_scheduler_dll::init_child_zone<Remote, Local>(
                transport_ctx, settings, std::move(wrapped_factory));
        };
        CO_RETURN rpc::error::OK();
    }
}

#endif
