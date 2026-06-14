/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#ifndef CANOPY_BUILD_COROUTINE

#  include <optional>
#  include <string>
#  include <utility>

#  include <json/convert.h>
#  include <rpc/module.h>
#  include <transports/blocking_dll/dll_transport.h>

namespace rpc
{
    struct object_module_init_params
    {
        rpc::blocking_dll::dll_init_params* params{};
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
        if (!module_params || !module_params->params || !module_params->params->input_descr)
            CO_RETURN rpc::error::INVALID_DATA();

        auto* params = module_params->params;
        if (params->input_descr->inbound_interface_id != Remote::get_id(rpc::get_version())
            || params->input_descr->outbound_interface_id != Local::get_id(rpc::get_version()))
            CO_RETURN rpc::error::OK();

        module_params->object_registered = true;
        std::optional<json::v1::object> module_global_settings;
        std::optional<rpc::module::startup_applications> startup_applications;
        if (params->module_settings_json && params->module_settings_json[0] != '\0')
        {
            try
            {
                module_global_settings.emplace(json::v1::parse(params->module_settings_json));
            }
            catch (...)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }
        }
        if (params->startup_applications_json && params->startup_applications_json[0] != '\0')
        {
            try
            {
                startup_applications.emplace(
                    json::v1::convert::from_json_object<rpc::module::startup_applications>(
                        json::v1::parse(params->startup_applications_json)));
            }
            catch (...)
            {
                CO_RETURN rpc::error::INVALID_DATA();
            }
        }

        auto wrapped_factory = rpc::module::make_child_service_factory<Remote, Local>(
            params->name ? std::string{params->name} : std::string{},
            module_global_settings ? &*module_global_settings : nullptr,
            rpc::module::make_object_factory<Remote, Local>(std::forward<Factory>(factory)),
            startup_applications ? &*startup_applications : nullptr);
        CO_RETURN rpc::blocking_dll::init_child_zone<Remote, Local>(params, std::move(wrapped_factory));
    }
}

#endif
