/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <cstddef>
#include <map>
#include <string>

#include <rpc/rpc.h>
#include <secure_coroutine_module/secure_coroutine_module.h>

#if !defined(FOR_SGX)
#  include <exception>
#  include <utility>

#  include <json/convert.h>
#endif

namespace rpc
{
    inline namespace v4
    {
        namespace secure_coroutine_module
        {
            constexpr std::size_t max_startup_application_count = 64;
            constexpr std::size_t max_startup_option_node_count = 512;
            constexpr std::size_t max_startup_option_depth = 16;
            constexpr std::size_t max_startup_option_key_bytes = 128;
            constexpr std::size_t max_startup_option_value_bytes = 4096;
            constexpr std::size_t max_startup_options_total_bytes = 64U * 1024U;

            using startup_applications = std::map<std::string, json::v1::object>;

            namespace detail
            {
                struct startup_option_validation_state
                {
                    std::size_t node_count = 1;
                    std::size_t total_bytes = 0;
                };

                inline int add_startup_option_bytes(
                    startup_option_validation_state& state,
                    std::size_t bytes)
                {
                    if (bytes > max_startup_options_total_bytes
                        || state.total_bytes > max_startup_options_total_bytes - bytes)
                        return rpc::error::RESOURCE_EXHAUSTED();
                    state.total_bytes += bytes;
                    return rpc::error::OK();
                }

                inline int validate_startup_option_resource_budget(
                    const json::v1::object& option,
                    startup_option_validation_state& state,
                    std::size_t depth)
                {
                    if (depth > max_startup_option_depth)
                        return rpc::error::RESOURCE_EXHAUSTED();

                    ++state.node_count;
                    if (state.node_count > max_startup_option_node_count)
                        return rpc::error::RESOURCE_EXHAUSTED();

                    switch (option.get_type())
                    {
                    case json::v1::object::type::string_type:
                    {
                        const auto value = option.get<std::string>();
                        if (value.size() > max_startup_option_value_bytes)
                            return rpc::error::INVALID_DATA();
                        return add_startup_option_bytes(state, value.size());
                    }
                    case json::v1::object::type::number_type:
                        return add_startup_option_bytes(state, 64);
                    case json::v1::object::type::bool_type:
                    case json::v1::object::type::null_type:
                        return rpc::error::OK();
                    case json::v1::object::type::array_type:
                        for (const auto& value : option.as_array())
                        {
                            auto error = validate_startup_option_resource_budget(value, state, depth + 1);
                            if (error != rpc::error::OK())
                                return error;
                        }
                        return rpc::error::OK();
                    case json::v1::object::type::map_type:
                        for (const auto& [key, value] : option.as_map())
                        {
                            if (key.empty() || key.size() > max_startup_option_key_bytes)
                                return rpc::error::INVALID_DATA();

                            auto error = add_startup_option_bytes(state, key.size());
                            if (error != rpc::error::OK())
                                return error;
                            error = validate_startup_option_resource_budget(value, state, depth + 1);
                            if (error != rpc::error::OK())
                                return error;
                        }
                        return rpc::error::OK();
                    }

                    return rpc::error::OK();
                }
            } // namespace detail

            inline int validate_startup_applications_resource_budget(const startup_applications& applications)
            {
                if (applications.size() > max_startup_application_count)
                    return rpc::error::RESOURCE_EXHAUSTED();

                detail::startup_option_validation_state state;
                for (const auto& [application_name, application_options] : applications)
                {
                    if (application_name.empty() || application_name.size() > max_startup_option_key_bytes)
                        return rpc::error::INVALID_DATA();

                    auto error = detail::add_startup_option_bytes(state, application_name.size());
                    if (error != rpc::error::OK())
                        return error;

                    error = detail::validate_startup_option_resource_budget(application_options, state, 1);
                    if (error != rpc::error::OK())
                        return error;
                }
                return rpc::error::OK();
            }

#if !defined(FOR_SGX)
            struct materialise_startup_applications_result
            {
                int error_code{rpc::error::OK()};
                startup_applications applications;
            };

            inline materialise_startup_applications_result materialise_startup_applications(
                const json::v1::object& client_options)
            {
                try
                {
                    if (client_options.get_type() == json::v1::object::type::null_type)
                        return {rpc::error::OK(), {}};

                    auto applications = json::v1::convert::from_json_object<startup_applications>(client_options);
                    const auto validation_error = validate_startup_applications_resource_budget(applications);
                    if (validation_error != rpc::error::OK())
                        return {validation_error, {}};
                    return {rpc::error::OK(), std::move(applications)};
                }
                catch (const std::exception&)
                {
                    return {rpc::error::INVALID_DATA(), {}};
                }
            }
#endif
        } // namespace secure_coroutine_module
    } // namespace v4
} // namespace rpc
