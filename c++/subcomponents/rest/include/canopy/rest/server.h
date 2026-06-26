/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <canopy/http_utils/http.h>
#include <canopy/rest/helpers.h>
#include <json/json_dom.h>
#include <rpc/rpc.h>

namespace canopy::rest
{
    struct server_request
    {
        std::string method;
        std::string target;
        std::vector<streaming::http_client::header> headers;
        std::string body;
    };

    struct server_response
    {
        int status_code{200};
        std::string content_type{"application/json"};
        std::string body;
        std::vector<streaming::http_client::header> headers;
    };

    struct path_match
    {
        bool matched{false};
        std::map<std::string, std::string> parameters;
    };

    [[nodiscard]] path_match match_path_template(
        std::string_view path,
        std::string_view pattern);
    [[nodiscard]] std::optional<std::string> first_query_parameter(
        std::string_view target,
        std::string_view name);
    [[nodiscard]] std::optional<std::string> first_header_value(
        const std::vector<streaming::http_client::header>& headers,
        std::string_view name);
    [[nodiscard]] json::v1::object component_to_json(std::string_view value);
    [[nodiscard]] std::vector<char> make_rpc_input_buffer(json::v1::map inputs);
    [[nodiscard]] std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::string_view out_param);
    [[nodiscard]] std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::string_view out_param,
        std::string_view field_map_json);
    [[nodiscard]] std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::initializer_list<response_field_binding> fields);
    [[nodiscard]] std::string response_body_from_rpc_output(
        std::string_view out_buf,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json);
    [[nodiscard]] std::string response_body_from_fields(
        json::v1::map output_values,
        std::initializer_list<response_field_binding> fields);
    [[nodiscard]] std::string response_body_from_fields(
        json::v1::map output_values,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json);
    [[nodiscard]] server_response json_response(
        int status_code,
        std::string body);
    [[nodiscard]] server_response error_response(
        int status_code,
        std::string message);
    void append_allowed_method(
        std::string& allowed_methods,
        std::string_view method);
    [[nodiscard]] server_response method_not_allowed_response(std::string allowed_methods);
    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body);
    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body,
        std::initializer_list<response_field_binding> fields);
    void add_body_input(
        json::v1::map& inputs,
        std::string_view name,
        std::string_view body,
        std::string_view field_map_json);
    [[nodiscard]] CORO_TASK(rpc::send_result) call_rpc_object(
        const rpc::casting_interface& object,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        json::v1::map inputs);
    [[nodiscard]] server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::string_view out_param);
    [[nodiscard]] server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::string_view out_param,
        std::string_view field_map_json);
    [[nodiscard]] server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::initializer_list<response_field_binding> fields);
    [[nodiscard]] server_response response_from_rpc_result(
        const rpc::send_result& result,
        std::initializer_list<response_field_binding> fields,
        std::string_view field_map_json);

    using server_handler = std::function<CORO_TASK(std::optional<server_response>)(const server_request&)>;

    class endpoint_registry
    {
    public:
        void add_endpoint(
            std::string base_path,
            server_handler handler,
            std::string name = {});

        template<class Handler>
        void add_generated_handler(
            Handler handler,
            std::string name = {})
        {
            add_endpoint(
                std::string(handler.base_path()),
                [handler = std::move(handler)](const server_request& request) -> CORO_TASK(std::optional<server_response>)
                { CO_RETURN CO_AWAIT handler.handle(request); },
                std::move(name));
        }

        template<class Interface>
        void add_object(
            std::string name,
            rpc::shared_ptr<Interface> object,
            std::string base_path = {})
        {
            using handler_info = typename Interface::rest_handler_info;
            static_assert(
                std::is_same_v<typename handler_info::interface_type, Interface>,
                "REST handler metadata must describe the interface being registered");

            if (name.empty())
                name = std::string(handler_info::default_name());
            if (base_path.empty())
                base_path = std::string(handler_info::default_base_path());
            add_generated_handler(
                typename Interface::rest_handler(std::move(object), std::move(base_path)), std::move(name));
        }

        template<class Interface>
        void add_object(
            rpc::shared_ptr<Interface> object,
            std::string base_path = {})
        {
            add_object<Interface>({}, std::move(object), std::move(base_path));
        }

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] bool may_handle(std::string_view target) const;

        CORO_TASK(std::optional<server_response>)
        handle(const server_request& request) const;

    private:
        struct endpoint
        {
            std::string base_path;
            std::string name;
            server_handler handler;
        };

        std::vector<endpoint> endpoints_;
    };

    namespace detail
    {
        template<typename T> struct rpc_optional_traits
        {
            static constexpr bool value = false;
        };

        template<typename T> struct rpc_optional_traits<rpc::optional<T>>
        {
            static constexpr bool value = true;
            using value_type = T;
        };

        template<typename T> struct rpc_optional_traits<rpc::nullable<T>>
        {
            static constexpr bool value = true;
            using value_type = T;
        };

        template<typename T> struct rpc_optional_traits<rpc::nullable_optional<T>>
        {
            static constexpr bool value = true;
            using value_type = T;
        };

        [[nodiscard]] json::v1::object bool_component_to_json(std::string_view value);
        [[nodiscard]] json::v1::object signed_component_to_json(std::string_view value);
        [[nodiscard]] json::v1::object unsigned_component_to_json(std::string_view value);
        [[nodiscard]] json::v1::object floating_component_to_json(std::string_view value);
    }

    template<typename T> [[nodiscard]] json::v1::object component_to_json_as(std::string_view value)
    {
        using value_type = std::remove_cv_t<std::remove_reference_t<T>>;
        if constexpr (detail::rpc_optional_traits<value_type>::value)
        {
            return component_to_json_as<typename detail::rpc_optional_traits<value_type>::value_type>(value);
        }
        else if constexpr (std::is_same_v<value_type, std::string>)
        {
            return json::v1::object(std::string(value));
        }
        else if constexpr (std::is_same_v<value_type, bool>)
        {
            return detail::bool_component_to_json(value);
        }
        else if constexpr (std::is_integral_v<value_type> && std::is_signed_v<value_type>
                           && !std::is_same_v<value_type, bool>)
        {
            return detail::signed_component_to_json(value);
        }
        else if constexpr (std::is_integral_v<value_type> && std::is_unsigned_v<value_type>)
        {
            return detail::unsigned_component_to_json(value);
        }
        else if constexpr (std::is_floating_point_v<value_type>)
        {
            return detail::floating_component_to_json(value);
        }
        else
        {
            return component_to_json(value);
        }
    }

    template<typename T> [[nodiscard]] T component_to_value(std::string_view value)
    {
        using ::json::v1::convert::from_json_object;
        return from_json_object<T>(component_to_json_as<T>(value));
    }

    template<typename T> [[nodiscard]] T body_to_value(std::string_view body)
    {
        return value_from_body<T>(body);
    }

    template<typename T>
    [[nodiscard]] T body_to_value(
        std::string_view body,
        std::initializer_list<response_field_binding> fields)
    {
        return value_from_body<T>(body, fields);
    }

    template<typename T>
    [[nodiscard]] T body_to_value(
        std::string_view body,
        std::string_view field_map_json)
    {
        return value_from_body<T>(body, field_map_json);
    }

    template<typename T>
    [[nodiscard]] T path_input_value(
        const path_match& path,
        std::string_view wire_name,
        bool required)
    {
        const auto value = path.parameters.find(std::string(wire_name));
        if (required && value == path.parameters.end())
            throw std::runtime_error("Required REST path parameter is missing");
        if (value == path.parameters.end())
            return T{};
        return component_to_value<T>(value->second);
    }

    template<typename T>
    [[nodiscard]] T query_input_value(
        std::string_view target,
        std::string_view wire_name,
        bool required)
    {
        const auto value = first_query_parameter(target, wire_name);
        if (required && !value)
            throw std::runtime_error("Required REST query parameter is missing");
        if (!value)
            return T{};
        return component_to_value<T>(*value);
    }

    template<typename T>
    [[nodiscard]] T header_input_value(
        const std::vector<streaming::http_client::header>& headers,
        std::string_view wire_name,
        bool required)
    {
        const auto value = first_header_value(headers, wire_name);
        if (required && !value)
            throw std::runtime_error("Required REST header parameter is missing");
        if (!value)
            return T{};
        return component_to_value<T>(*value);
    }

    template<typename T>
    void add_path_input(
        json::v1::map& inputs,
        std::string_view input_name,
        const path_match& path,
        std::string_view wire_name,
        bool required)
    {
        const auto value = path.parameters.find(std::string(wire_name));
        if (required && value == path.parameters.end())
            throw std::runtime_error("Required REST path parameter is missing");
        if (value != path.parameters.end())
            inputs.emplace(std::string(input_name), component_to_json_as<T>(value->second));
    }

    template<typename T>
    void add_query_input(
        json::v1::map& inputs,
        std::string_view input_name,
        std::string_view target,
        std::string_view wire_name,
        bool required)
    {
        const auto value = first_query_parameter(target, wire_name);
        if (required && !value)
            throw std::runtime_error("Required REST query parameter is missing");
        if (value)
            inputs.emplace(std::string(input_name), component_to_json_as<T>(*value));
    }

    template<typename T>
    void add_header_input(
        json::v1::map& inputs,
        std::string_view input_name,
        const std::vector<streaming::http_client::header>& headers,
        std::string_view wire_name,
        bool required)
    {
        const auto value = first_header_value(headers, wire_name);
        if (required && !value)
            throw std::runtime_error("Required REST header parameter is missing");
        if (value)
            inputs.emplace(std::string(input_name), component_to_json_as<T>(*value));
    }
}
