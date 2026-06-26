/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <json/convert.h>
#include <json/json_dom.h>
#include <streaming/http_client/client.h>

namespace canopy::rest
{
    struct response_field_binding
    {
        std::string_view output_name;
        std::string_view wire_name;
        bool required{true};
    };

    [[nodiscard]] std::string normalise_json(std::string_view text);
    void add_json_headers(
        std::vector<streaming::http_client::header>& headers,
        bool has_body);
    [[nodiscard]] std::string encode_path_segment(std::string_view value);
    [[nodiscard]] std::string append_path_segment(
        std::string_view base_path,
        std::string_view segment);
    [[nodiscard]] std::string join_target(
        std::string_view base_path,
        std::string_view operation_path);
    void append_query_parameter(
        std::string& target,
        std::string_view name,
        std::string_view value);
    [[nodiscard]] std::string required_string_field(
        std::string_view json_text,
        std::string_view field_name);
    [[nodiscard]] json::v1::map rpc_input_map(const std::vector<char>& buffer);
    [[nodiscard]] const json::v1::object& required_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] const json::v1::object* optional_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] bool is_null(const json::v1::object& value);
    [[nodiscard]] std::string component_string(const json::v1::object& value);
    [[nodiscard]] std::string replace_path_parameter(
        std::string target,
        std::string_view name,
        std::string_view value);
    [[nodiscard]] std::string transform_body_to_wire(
        std::string_view body,
        std::string_view field_map_json);
    [[nodiscard]] std::string transform_body_to_idl(
        std::string_view body,
        std::string_view field_map_json);
    [[nodiscard]] std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name);
    [[nodiscard]] std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name,
        std::initializer_list<response_field_binding> fields);
    [[nodiscard]] std::string body_from_rpc_input(
        const json::v1::map& input,
        std::string_view name,
        std::string_view field_map_json);
    [[nodiscard]] std::string wrap_response_body(
        std::string_view output_name,
        std::string_view body);
    [[nodiscard]] std::string wrap_response_body(
        std::string_view output_name,
        std::string_view body,
        std::string_view field_map_json);
    [[nodiscard]] std::string wrap_response_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body);
    [[nodiscard]] std::string wrap_response_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body,
        std::string_view field_map_json);
    [[nodiscard]] json::v1::map response_fields_from_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body);
    [[nodiscard]] json::v1::map response_fields_from_body(
        std::initializer_list<response_field_binding> fields,
        std::string_view body,
        std::string_view field_map_json);

    template<typename T> [[nodiscard]] json::v1::object to_json_value(const T& value)
    {
        using ::json::v1::convert::to_json_object;
        return to_json_object(value);
    }

    template<typename T> [[nodiscard]] std::string to_json_string(const T& value)
    {
        return ::json::v1::dump(to_json_value(value));
    }

    template<typename T> [[nodiscard]] T from_json_string(std::string_view text)
    {
        return ::json::v1::convert::from_json_object<T>(::json::v1::parse(text));
    }

    template<typename T> [[nodiscard]] std::string component_string_from_value(const T& value)
    {
        return component_string(to_json_value(value));
    }

    template<typename T> [[nodiscard]] std::string body_from_value(const T& value)
    {
        return ::json::v1::dump(to_json_value(value));
    }

    template<typename T>
    [[nodiscard]] std::string body_from_value(
        const T& value,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
            return body_from_value(value);

        const auto body = to_json_value(value);
        if (body.get_type() != json::v1::object::type::map_type)
            throw std::runtime_error("REST body value is not an object");

        const auto& body_fields = body.as_map();
        json::v1::map wire_fields;
        for (const auto& field : fields)
        {
            const auto item = body_fields.find(std::string(field.output_name));
            if (item == body_fields.end())
            {
                if (field.required)
                    throw std::runtime_error("required REST request field is missing");
                continue;
            }
            if (!field.required && is_null(item->second))
                continue;
            wire_fields.emplace(std::string(field.wire_name), item->second);
        }
        return ::json::v1::dump(json::v1::object(std::move(wire_fields)));
    }

    template<typename T>
    [[nodiscard]] std::string body_from_value(
        const T& value,
        std::string_view field_map_json)
    {
        return transform_body_to_wire(body_from_value(value), field_map_json);
    }

    template<typename T> [[nodiscard]] T value_from_body(std::string_view body)
    {
        return from_json_string<T>(body.empty() ? "null" : body);
    }

    template<typename T>
    [[nodiscard]] T value_from_body(
        std::string_view body,
        std::initializer_list<response_field_binding> fields)
    {
        if (fields.size() == 0)
            return value_from_body<T>(body);

        const auto request_body = json::v1::parse(body.empty() ? "{}" : body);
        if (request_body.get_type() != json::v1::object::type::map_type)
            throw std::runtime_error("REST request body is not an object");

        const auto& wire_fields = request_body.as_map();
        json::v1::map idl_fields;
        for (const auto& field : fields)
        {
            const auto item = wire_fields.find(std::string(field.wire_name));
            if (item == wire_fields.end())
            {
                if (field.required)
                    throw std::runtime_error("required REST request field is missing");
                continue;
            }
            idl_fields.emplace(std::string(field.output_name), item->second);
        }
        return ::json::v1::convert::from_json_object<T>(json::v1::object(std::move(idl_fields)));
    }

    template<typename T>
    [[nodiscard]] T value_from_body(
        std::string_view body,
        std::string_view field_map_json)
    {
        const auto transformed = transform_body_to_idl(body.empty() ? "{}" : body, field_map_json);
        return ::json::v1::convert::from_json_object<T>(::json::v1::parse(transformed));
    }

    template<typename T>
    void assign_response_field(
        T& output,
        const json::v1::map& fields,
        std::string_view name,
        bool required)
    {
        const auto item = fields.find(std::string(name));
        if (item == fields.end())
        {
            if (required)
                throw std::runtime_error("required REST response field is missing");
            return;
        }
        output = ::json::v1::convert::from_json_object<T>(item->second);
    }

    template<typename T>
    void add_response_field(
        json::v1::map& fields,
        std::string_view name,
        const T& value,
        bool required)
    {
        auto json_value = to_json_value(value);
        if (!required && is_null(json_value))
            return;
        fields.emplace(std::string(name), std::move(json_value));
    }
} // namespace canopy::rest
